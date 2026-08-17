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

#include "logging.hpp"
#include "protocol.hpp"
#include "protocol_test_client.hpp"
#include "server_test_harness.hpp"

#include "client.pb.h"
#include "wire.pb.h"

#include <boost/asio/post.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <string>
#include <string_view>
#include <utility>

using ::testing::_;
using ::testing::Return;

namespace {

[[maybe_unused]] std::string replace_once(std::string document, std::string_view needle, std::string_view replacement)
{
    auto position = document.find(needle);
    EXPECT_NE(position, std::string::npos) << needle;
    if (position != std::string::npos) {
        document.replace(position, needle.size(), replacement);
    }
    return document;
}

} // anonymous namespace

class ServerReloadIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        original_log_level_ = logging::active_level();
        const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
        config_path_ = std::filesystem::temp_directory_path()
            / std::format("storage-lender-reload-{}-{}.toml", ::getpid(), test->name());
    }

    void TearDown() override
    {
        harness_.stop();
        logging::set_level(original_log_level_);
        std::error_code error;
        std::filesystem::remove(config_path_, error);
    }

    void start(ServerConfig config, PeerCredentialReader reader, std::string config_path = { })
    {
        if (config_path.empty()) {
            config_path = config_path_.string();
        }
        auto started = harness_.start(std::move(config), std::move(reader), std::move(config_path), "reload");
        ASSERT_TRUE(started.has_value()) << started.error().message();
    }

    UsageSnapshot snapshot() { return harness_.usage_snapshot(); }

    bool wait_for_usage(const std::function<bool(const UsageSnapshot&)>& predicate)
    {
        return harness_.wait_for_usage(predicate);
    }

    static void expect_alive(ProtocolTestClient& client)
    {
        ASSERT_TRUE(client.send_request(999));
        nvidia::storage_lender::wire::v1::Response response;
        ASSERT_TRUE(client.recv_response(response));
        EXPECT_EQ(response.status_code(), static_cast<int32_t>(StatusCode::INVALID_ARGUMENT));
    }

    static nvidia::storage_lender::wire::v1::Response call(
        ProtocolTestClient& client, MethodId method, const google::protobuf::MessageLite& request)
    {
        auto response = client.call(method, request);
        EXPECT_TRUE(response.has_value());
        return response ? std::move(*response) : nvidia::storage_lender::wire::v1::Response { };
    }

    static nvidia::storage_lender::wire::v1::Response call(ProtocolTestClient& client, MethodId method)
    {
        auto response = client.call(method);
        EXPECT_TRUE(response.has_value());
        return response ? std::move(*response) : nvidia::storage_lender::wire::v1::Response { };
    }

    static uint32_t open_device(ProtocolTestClient& client, std::string_view pci_address)
    {
        auto device_id = client.open_shared_device(pci_address);
        EXPECT_TRUE(device_id.has_value());
        return device_id.value_or(0);
    }

    bool write_config(std::string_view config)
    {
        std::ofstream output { config_path_, std::ios::binary | std::ios::trunc };
        output << config;
        output.close();
        return output.good();
    }

    void request_reload_and_wait(bool request_twice = false)
    {
        std::promise<void> complete;
        auto future = complete.get_future();
        boost::asio::post(harness_.io_context(), [this, request_twice, &complete] {
            harness_.server().request_reload();
            if (request_twice) {
                harness_.server().request_reload();
            }
            boost::asio::post(harness_.io_context(), [&complete] { complete.set_value(); });
        });
        future.get();
    }

    std::string socket_config()
    {
        const auto& api_socket = harness_.socket_config();
        const auto& ctl_socket = harness_.ctl_socket_config();
        return std::format(R"([sockets.api]
path = "{}"
owner = "{}"
group = "{}"
mode = 0o{:o}

[sockets.ctl]
path = "{}"
owner = "{}"
group = "{}"
mode = 0o{:o}

)",
            api_socket.path, api_socket.owner, api_socket.group, api_socket.mode, ctl_socket.path, ctl_socket.owner,
            ctl_socket.group, ctl_socket.mode);
    }

    static std::string logging_config(std::string_view level)
    {
        if (level.empty()) {
            return { };
        }
        return std::format("[logging]\nlevel = \"{}\"\n\n", level);
    }

    std::string enforced_config(std::string_view default_device_handles, std::string_view principal_tables = { },
        std::string_view logging_level = { })
    {
        return std::format(R"(schema_version = 1

{}
{}
[devices.default]
num_io_queues = 65534
admin_command_timeout_ms = 10000

[quotas]
mode = "enforced"

[quotas.global]
sessions = "unlimited"
transferred_fds = "unlimited"
mapped_buffers = "unlimited"
max_buffer_bytes = "unlimited"
mapped_bytes = "unlimited"
device_handles = "unlimited"
completion_queues = "unlimited"
submission_queues = "unlimited"

[quotas.default]
sessions = "unlimited"
transferred_fds = "unlimited"
mapped_buffers = "unlimited"
max_buffer_bytes = "unlimited"
mapped_bytes = "unlimited"
device_handles = {}
completion_queues = "unlimited"
submission_queues = "unlimited"

{})",
            logging_config(logging_level), socket_config(), default_device_handles, principal_tables);
    }

    std::string unlimited_config(std::string_view logging_level = { })
    {
        return std::format(R"(schema_version = 1

{}
{}
[devices.default]
num_io_queues = 65534
admin_command_timeout_ms = 10000

[quotas]
mode = "unlimited"
)",
            logging_config(logging_level), socket_config());
    }

    ServerTestHarness harness_;
    void* const CTRLR = reinterpret_cast<void*>(0xCAFE);
    std::filesystem::path config_path_;
    LogLevel original_log_level_ { LogLevel::INFO };
};

TEST_F(ServerReloadIntegrationTest, CreatedFileAfterStartupCanReloadWhenRestartFieldsMatch)
{
    start(unlimited_server_config(),
        sequential_test_peer_credentials({ PeerCredentials { .pid = 1, .uid = 1000, .gid = 1000 } }),
        config_path_.string());
    ASSERT_TRUE(write_config(enforced_config("0")));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    expect_alive(*client);
    request_reload_and_wait();

    nvidia::storage_lender::v1::OpenDeviceRequest request;
    request.set_pci_address("0000:01:00.0");
    request.set_open_mode(nvidia::storage_lender::v1::SHARED);
    const auto response = call(*client, MethodId::OPEN_DEVICE, request);
    EXPECT_EQ(response.status_code(), static_cast<int32_t>(StatusCode::RESOURCE_EXHAUSTED));
    EXPECT_EQ(snapshot().generation, 2u);
}

TEST_F(ServerReloadIntegrationTest, MissingFileRejectsReloadAndPreservesActivePolicy)
{
    start(unlimited_server_config(), default_test_peer_credentials(), config_path_.string());
    ASSERT_FALSE(std::filesystem::exists(config_path_));

    request_reload_and_wait();

    const auto state = snapshot();
    EXPECT_EQ(state.generation, 1u);
    EXPECT_EQ(state.mode, QuotaMode::UNLIMITED);
    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    expect_alive(*client);
}

TEST_F(ServerReloadIntegrationTest, InvalidReloadRetainsPreviousPolicy)
{
    start(unlimited_server_config(),
        sequential_test_peer_credentials({ PeerCredentials { .pid = 1, .uid = 1000, .gid = 1000 } }),
        config_path_.string());
    ASSERT_TRUE(write_config("not valid TOML = ["));
    EXPECT_CALL(harness_.backend(), nvme_connect("0000:01:00.0", _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), nvme_detach(CTRLR)).WillOnce(Return(std::expected<void, int> { }));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    expect_alive(*client);
    request_reload_and_wait();

    const auto device_id = open_device(*client, "0000:01:00.0");
    EXPECT_NE(device_id, 0u);
    EXPECT_EQ(snapshot().generation, 1u);

    nvidia::storage_lender::v1::CloseDeviceRequest close;
    close.set_device_id(device_id);
    EXPECT_EQ(call(*client, MethodId::CLOSE_DEVICE, close).status_code(), static_cast<int32_t>(StatusCode::OK));
}

TEST_F(ServerReloadIntegrationTest, ValidReloadChangesLoggingLevelWithoutDroppingSession)
{
    auto config = unlimited_server_config();
    config.logging.level = LogLevel::INFO;
    start(std::move(config), default_test_peer_credentials(), config_path_.string());
    ASSERT_TRUE(write_config(unlimited_config("debug")));

    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    expect_alive(*client);

    request_reload_and_wait();

    EXPECT_EQ(logging::active_level(), LogLevel::DEBUG);
    EXPECT_EQ(snapshot().generation, 2u);
    expect_alive(*client);
}

TEST_F(ServerReloadIntegrationTest, DevicePolicyChangesRejectEntireReload)
{
    auto config = unlimited_server_config();
    config.logging.level = LogLevel::INFO;
    start(std::move(config), default_test_peer_credentials(), config_path_.string());

    auto candidate = unlimited_config("debug");
    candidate = replace_once(std::move(candidate), "num_io_queues = 65534", "num_io_queues = 2048");
    ASSERT_TRUE(write_config(candidate));
    request_reload_and_wait();

    EXPECT_EQ(logging::active_level(), LogLevel::INFO);
    EXPECT_EQ(snapshot().generation, 1u);
}

TEST_F(ServerReloadIntegrationTest, UnavailableLoggingLevelRejectsEntireReload)
{
    if (logging::is_available(LogLevel::TRACE)) {
        GTEST_SKIP() << "trace is available in this build";
    }

    auto config = unlimited_server_config();
    config.logging.level = LogLevel::INFO;
    start(std::move(config), default_test_peer_credentials(), config_path_.string());
    ASSERT_TRUE(write_config(enforced_config("0", { }, "trace")));
    EXPECT_CALL(harness_.backend(), nvme_connect("0000:01:00.0", _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), nvme_detach(CTRLR)).WillOnce(Return(std::expected<void, int> { }));

    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    request_reload_and_wait();

    EXPECT_EQ(logging::active_level(), LogLevel::INFO);
    EXPECT_EQ(snapshot().generation, 1u);
    const auto device_id = open_device(*client, "0000:01:00.0");
    EXPECT_NE(device_id, 0u);

    nvidia::storage_lender::v1::CloseDeviceRequest close;
    close.set_device_id(device_id);
    EXPECT_EQ(call(*client, MethodId::CLOSE_DEVICE, close).status_code(), static_cast<int32_t>(StatusCode::OK));
}

TEST_F(ServerReloadIntegrationTest, SocketChangesRejectEntireReload)
{
    auto config = unlimited_server_config();
    config.principal_resolver = PrincipalResolver { { { 1000, "original" } }, { } };
    start(std::move(config), default_test_peer_credentials(), config_path_.string());

    constexpr std::string_view PRINCIPALS = R"([principals.changed]
uids = [1000]
)";

    struct Case {
        std::string original;
        std::string replacement;
    };
    const auto& socket = harness_.socket_config();
    const Case cases[] = {
        { std::format("path = \"{}\"", socket.path), "path = \"/run/storage-lender/changed.sock\"" },
        { std::format("owner = \"{}\"", socket.owner), "owner = \"changed-owner\"" },
        { std::format("group = \"{}\"", socket.group), "group = \"changed-group\"" },
        { std::format("mode = 0o{:o}", socket.mode), socket.mode == 0600 ? "mode = 0o660" : "mode = 0o600" },
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.replacement);
        auto candidate = enforced_config("0", PRINCIPALS);
        candidate = replace_once(std::move(candidate), test_case.original, test_case.replacement);
        ASSERT_TRUE(write_config(candidate));
        request_reload_and_wait();
        EXPECT_EQ(snapshot().generation, 1u);

        {
            auto client = harness_.connect_protocol();
            ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
            expect_alive(*client);

            const auto usage = snapshot();
            const auto original = usage.principal_usage.find("original");
            ASSERT_NE(original, usage.principal_usage.end());
            EXPECT_EQ(original->second.active.sessions, 1u);
            EXPECT_FALSE(usage.principal_usage.contains("changed"));
        }
        ASSERT_TRUE(wait_for_usage([](const UsageSnapshot& usage) { return usage.principal_usage.empty(); }));
    }
}

TEST_F(ServerReloadIntegrationTest, CtlSocketChangesRejectEntireReload)
{
    start(unlimited_server_config(), default_test_peer_credentials(), config_path_.string());

    const auto& socket = harness_.ctl_socket_config();
    auto candidate = enforced_config("0");
    candidate = replace_once(std::move(candidate), std::format("path = \"{}\"", socket.path),
        "path = \"/run/storage-lender/changed-ctl.sock\"");
    ASSERT_TRUE(write_config(candidate));

    request_reload_and_wait();

    EXPECT_EQ(snapshot().generation, 1u);
}

TEST_F(ServerReloadIntegrationTest, LowerLimitGrandfathersExistingResources)
{
    start(unlimited_server_config(),
        sequential_test_peer_credentials({ PeerCredentials { .pid = 1, .uid = 1000, .gid = 1000 } }),
        config_path_.string());
    ASSERT_TRUE(write_config(enforced_config("0")));
    EXPECT_CALL(harness_.backend(), nvme_connect("0000:01:00.0", _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), nvme_detach(CTRLR)).WillOnce(Return(std::expected<void, int> { }));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    const auto device_id = open_device(*client, "0000:01:00.0");
    request_reload_and_wait();

    nvidia::storage_lender::v1::OpenDeviceRequest open;
    open.set_pci_address("0000:01:00.0");
    open.set_open_mode(nvidia::storage_lender::v1::SHARED);
    EXPECT_EQ(
        call(*client, MethodId::OPEN_DEVICE, open).status_code(), static_cast<int32_t>(StatusCode::RESOURCE_EXHAUSTED));

    nvidia::storage_lender::v1::CloseDeviceRequest close;
    close.set_device_id(device_id);
    EXPECT_EQ(call(*client, MethodId::CLOSE_DEVICE, close).status_code(), static_cast<int32_t>(StatusCode::OK));
}

TEST_F(ServerReloadIntegrationTest, MappingChangesAffectOnlyNewSessions)
{
    constexpr std::string_view PRINCIPALS = R"([principals.camera]
uids = [1002]
[principals.camera.quotas]
device_handles = 1

[principals.storage]
uids = [1001]
[principals.storage.quotas]
device_handles = 0
)";

    QuotaLimits camera_limits;
    camera_limits.device_handles = 1;
    QuotaLimits storage_limits;
    storage_limits.device_handles = 0;
    QuotaPolicy policy;
    policy.mode = QuotaMode::ENFORCED;
    policy.principals = { { "camera", camera_limits }, { "storage", storage_limits } };
    PrincipalResolver resolver { { { 1001, "camera" }, { 1002, "storage" } }, { } };
    start(
        ServerConfig {
            .api_socket = { },
            .device_policy = test_device_policy(),
            .quota_policy = std::move(policy),
            .principal_resolver = std::move(resolver),
        },
        sequential_test_peer_credentials({ PeerCredentials { .pid = 1, .uid = 1001, .gid = 1000 },
            PeerCredentials { .pid = 2, .uid = 1001, .gid = 1000 } }),
        config_path_.string());
    ASSERT_TRUE(write_config(enforced_config("\"unlimited\"", PRINCIPALS, "debug")));
    EXPECT_CALL(harness_.backend(), nvme_connect("0000:01:00.0", _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), nvme_detach(CTRLR)).WillOnce(Return(std::expected<void, int> { }));

    auto existing = harness_.connect_protocol();

    ASSERT_TRUE(existing.has_value()) << "protocol connect failed (errno=" << existing.error() << ')';
    expect_alive(*existing);
    request_reload_and_wait();
    EXPECT_EQ(logging::active_level(), LogLevel::DEBUG);
    auto added = harness_.connect_protocol();
    ASSERT_TRUE(added.has_value()) << "protocol connect failed (errno=" << added.error() << ')';
    expect_alive(*added);

    const auto device_id = open_device(*existing, "0000:01:00.0");
    nvidia::storage_lender::v1::OpenDeviceRequest open;
    open.set_pci_address("0000:01:00.0");
    open.set_open_mode(nvidia::storage_lender::v1::SHARED);
    EXPECT_EQ(
        call(*added, MethodId::OPEN_DEVICE, open).status_code(), static_cast<int32_t>(StatusCode::RESOURCE_EXHAUSTED));

    nvidia::storage_lender::v1::CloseDeviceRequest close;
    close.set_device_id(device_id);
    EXPECT_EQ(call(*existing, MethodId::CLOSE_DEVICE, close).status_code(), static_cast<int32_t>(StatusCode::OK));
}

TEST_F(ServerReloadIntegrationTest, ActivePrincipalRemovalIsRejected)
{
    QuotaPolicy policy;
    policy.mode = QuotaMode::ENFORCED;
    policy.principals = { { "camera", QuotaLimits { } } };
    PrincipalResolver resolver { { { 1001, "camera" } }, { } };
    start(
        ServerConfig {
            .api_socket = { },
            .device_policy = test_device_policy(),
            .quota_policy = std::move(policy),
            .principal_resolver = std::move(resolver),
        },
        sequential_test_peer_credentials({ PeerCredentials { .pid = 1, .uid = 1001, .gid = 1000 } }),
        config_path_.string());
    ASSERT_TRUE(write_config(unlimited_config()));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    expect_alive(*client);
    ASSERT_TRUE(wait_for_usage([](const UsageSnapshot& usage) {
        const auto camera = usage.principal_usage.find("camera");
        return camera != usage.principal_usage.end() && camera->second.active.sessions == 1;
    }));
    request_reload_and_wait();

    EXPECT_EQ(snapshot().generation, 1u);
}

TEST_F(ServerReloadIntegrationTest, PendingReloadRequestsCoalesce)
{
    start(unlimited_server_config(),
        sequential_test_peer_credentials({ PeerCredentials { .pid = 1, .uid = 1000, .gid = 1000 } }),
        config_path_.string());
    ASSERT_TRUE(write_config(unlimited_config()));

    EXPECT_EQ(snapshot().generation, 1u);
    request_reload_and_wait(true);
    EXPECT_EQ(snapshot().generation, 2u);
}
