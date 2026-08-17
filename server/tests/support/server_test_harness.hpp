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

#pragma once

#include "mock_nvme_backend.hpp"
#include "protocol_test_client.hpp"
#include "server.hpp"

#include <gmock/gmock.h>

#include <boost/asio/io_context.hpp>

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

DevicePolicy test_device_policy();
ServerConfig unlimited_server_config();
PeerCredentialReader default_test_peer_credentials();
PeerCredentialReader sequential_test_peer_credentials(std::vector<PeerCredentials> peers);

class ServerTestHarness {
public:
    ServerTestHarness();
    ~ServerTestHarness();

    ServerTestHarness(const ServerTestHarness&) = delete;
    ServerTestHarness& operator=(const ServerTestHarness&) = delete;

    std::expected<void, SocketEndpointError> start();
    std::expected<void, SocketEndpointError> start(ServerConfig config, PeerCredentialReader credential_reader,
        std::string config_path = { }, std::string_view socket_tag = "server");
    void stop();

    std::expected<ProtocolTestClient, int> connect_protocol() const;
    UsageSnapshot usage_snapshot();
    bool wait_for_usage(const std::function<bool(const UsageSnapshot&)>& predicate);

    ::testing::NiceMock<MockNvmeBackend>& backend();
    DeviceManager& device_manager();
    boost::asio::io_context& io_context();
    StorageLenderServer& server();
    const std::string& socket_path() const;
    const SocketConfig& socket_config() const;
    const std::string& ctl_socket_path() const;
    const SocketConfig& ctl_socket_config() const;

private:
    std::expected<void, SocketEndpointError> prepare_socket_config(std::string_view tag);

    ::testing::NiceMock<MockNvmeBackend> backend_;
    std::unique_ptr<DeviceManager> device_manager_;
    boost::asio::io_context io_context_;
    SocketConfig socket_config_;
    SocketConfig ctl_socket_config_;
    std::filesystem::path socket_directory_;
    std::unique_ptr<StorageLenderServer> server_;
    std::thread server_thread_;
};
