/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "server_test_harness.hpp"

#include <boost/asio/post.hpp>

#include <grp.h>
#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <future>
#include <ranges>
#include <utility>

DevicePolicy test_device_policy()
{
    return DevicePolicy {
        .default_options = {
            .num_io_queues = 65534,
            .admin_command_timeout = std::chrono::milliseconds { 10000 },
        },
        .overrides = { },
    };
}

ServerConfig unlimited_server_config()
{
    return ServerConfig {
        .api_socket = { },
        .device_policy = test_device_policy(),
        .quota_policy = QuotaPolicy { },
        .principal_resolver = PrincipalResolver { { }, { } },
    };
}

PeerCredentialReader default_test_peer_credentials()
{
    return [](int) -> std::expected<PeerCredentials, LenderError> {
        return PeerCredentials { .pid = 1234, .uid = 1000, .gid = 1000 };
    };
}

PeerCredentialReader sequential_test_peer_credentials(std::vector<PeerCredentials> peers)
{
    struct State {
        std::vector<PeerCredentials> peers;
        std::size_t next { };
    };

    auto state = std::make_shared<State>(State { .peers = std::move(peers) });
    return [state](int) -> std::expected<PeerCredentials, LenderError> {
        if (state->peers.empty()) {
            return std::unexpected(LenderError::INTERNAL);
        }
        const auto index = std::min(state->next++, state->peers.size() - 1);
        return state->peers[index];
    };
}

ServerTestHarness::ServerTestHarness() = default;

ServerTestHarness::~ServerTestHarness() { stop(); }

std::expected<void, SocketEndpointError> ServerTestHarness::start()
{
    return start(unlimited_server_config(), default_test_peer_credentials());
}

std::expected<void, SocketEndpointError> ServerTestHarness::start(
    ServerConfig config, PeerCredentialReader credential_reader, std::string config_path, std::string_view socket_tag)
{
    if (server_) {
        return std::unexpected(SocketEndpointError { "start test server", socket_config_.path, "already running" });
    }

    auto prepared = prepare_socket_config(socket_tag);
    if (!prepared) {
        return std::unexpected(prepared.error());
    }

    io_context_.restart();
    config.api_socket = socket_config_;
    config.ctl_socket = ctl_socket_config_;
    auto api_endpoint = UnixSocketEndpoint::create(io_context_, socket_config_);
    if (!api_endpoint) {
        return std::unexpected(api_endpoint.error());
    }
    auto ctl_endpoint = UnixSocketEndpoint::create(io_context_, ctl_socket_config_);
    if (!ctl_endpoint) {
        return std::unexpected(ctl_endpoint.error());
    }
    device_manager_ = std::make_unique<DeviceManager>(backend_, config.device_policy);
    server_ = std::make_unique<StorageLenderServer>(io_context_, std::move(*api_endpoint), std::move(*ctl_endpoint),
        backend_, *device_manager_, std::move(config), std::move(config_path), std::move(credential_reader));
    server_thread_ = std::thread([this] { io_context_.run(); });
    return { };
}

void ServerTestHarness::stop()
{
    if (server_) {
        boost::asio::post(io_context_, [server = server_.get()] { server->shutdown(); });
    }
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    server_.reset();
    device_manager_.reset();

    if (!socket_config_.path.empty()) {
        std::error_code error;
        const auto socket_exists = std::filesystem::exists(socket_config_.path, error);
        EXPECT_FALSE(error) << error.message();
        EXPECT_FALSE(socket_exists);
    }

    if (!ctl_socket_config_.path.empty()) {
        std::error_code error;
        const auto socket_exists = std::filesystem::exists(ctl_socket_config_.path, error);
        EXPECT_FALSE(error) << error.message();
        EXPECT_FALSE(socket_exists);
    }

    if (!socket_directory_.empty()) {
        std::error_code error;
        std::filesystem::remove(socket_directory_, error);
        EXPECT_FALSE(error) << error.message();
        socket_directory_.clear();
    }
    socket_config_ = { };
    ctl_socket_config_ = { };
}

std::expected<ProtocolTestClient, int> ServerTestHarness::connect_protocol() const
{
    return ProtocolTestClient::connect(socket_config_.path);
}

UsageSnapshot ServerTestHarness::usage_snapshot()
{
    std::promise<UsageSnapshot> result;
    auto future = result.get_future();
    boost::asio::post(io_context_, [this, &result] { result.set_value(server_->usage_snapshot()); });
    return future.get();
}

bool ServerTestHarness::wait_for_usage(const std::function<bool(const UsageSnapshot&)>& predicate)
{
    return std::ranges::any_of(std::views::iota(0, 100), [this, &predicate](int) {
        if (predicate(usage_snapshot())) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return false;
    });
}

::testing::NiceMock<MockNvmeBackend>& ServerTestHarness::backend() { return backend_; }

DeviceManager& ServerTestHarness::device_manager() { return *device_manager_; }

boost::asio::io_context& ServerTestHarness::io_context() { return io_context_; }

StorageLenderServer& ServerTestHarness::server() { return *server_; }

const std::string& ServerTestHarness::socket_path() const { return socket_config_.path; }

const SocketConfig& ServerTestHarness::socket_config() const { return socket_config_; }

const std::string& ServerTestHarness::ctl_socket_path() const { return ctl_socket_config_.path; }

const SocketConfig& ServerTestHarness::ctl_socket_config() const { return ctl_socket_config_; }

std::expected<void, SocketEndpointError> ServerTestHarness::prepare_socket_config(std::string_view tag)
{
    errno = 0;
    const auto* owner = ::getpwuid(::geteuid());
    if (owner == nullptr) {
        return std::unexpected(SocketEndpointError { "resolve test socket owner", std::to_string(::geteuid()),
            errno == 0 ? "account not found" : ::strerror(errno) });
    }
    const std::string owner_name { owner->pw_name };

    errno = 0;
    const auto* group = ::getgrgid(::getegid());
    if (group == nullptr) {
        return std::unexpected(SocketEndpointError { "resolve test socket group", std::to_string(::getegid()),
            errno == 0 ? "account not found" : ::strerror(errno) });
    }
    const std::string group_name { group->gr_name };

    std::error_code error;
    const auto temp_directory = std::filesystem::temp_directory_path(error);
    if (error) {
        return std::unexpected(
            SocketEndpointError { "prepare test socket directory", socket_directory_.string(), error.message() });
    }

    static std::atomic<uint64_t> next_id { };
    socket_directory_ = temp_directory / std::format("storage-lender-{}-{}-{}", tag, ::getpid(), next_id.fetch_add(1));
    if (!std::filesystem::create_directory(socket_directory_, error)) {
        if (!error) {
            error = std::make_error_code(std::errc::file_exists);
        }
        return std::unexpected(
            SocketEndpointError { "prepare test socket directory", socket_directory_.string(), error.message() });
    }

    std::filesystem::permissions(
        socket_directory_, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
    if (error) {
        return std::unexpected(
            SocketEndpointError { "prepare test socket directory", socket_directory_.string(), error.message() });
    }

    socket_config_ = SocketConfig {
        .path = (socket_directory_ / "api.sock").string(),
        .owner = owner_name,
        .group = group_name,
        .mode = 0660,
    };
    ctl_socket_config_ = SocketConfig {
        .path = (socket_directory_ / "ctl.sock").string(),
        .owner = owner_name,
        .group = group_name,
        .mode = 0600,
    };
    return { };
}
