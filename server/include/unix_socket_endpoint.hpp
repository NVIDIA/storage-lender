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

#include "socket_config.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>

#include <expected>
#include <functional>
#include <string>
#include <string_view>

#include <sys/stat.h>

struct SocketEndpointError {
    std::string operation;
    std::string target;
    std::string detail;

    std::string message() const;
};

enum class SocketEndpointStage { BOUND_UNINSPECTED, BOUND, OWNED, MODE_SET, VERIFIED, LISTENING };
using SocketEndpointCheckpoint = std::function<std::expected<void, SocketEndpointError>(SocketEndpointStage)>;

namespace socket_endpoint_detail {
enum class DirectoryRole { ANCESTOR, PARENT };
std::expected<void, SocketEndpointError> validate_directory_metadata(
    const struct stat& metadata, uid_t effective_uid, DirectoryRole role, std::string_view path);
}

class UnixSocketEndpoint {
public:
    using Acceptor = boost::asio::local::stream_protocol::acceptor;

    static std::expected<UnixSocketEndpoint, SocketEndpointError> create(
        boost::asio::io_context& ioc, const SocketConfig& config);
    static std::expected<UnixSocketEndpoint, SocketEndpointError> create_for_test(
        boost::asio::io_context& ioc, const SocketConfig& config, SocketEndpointCheckpoint checkpoint);

    ~UnixSocketEndpoint();
    UnixSocketEndpoint(UnixSocketEndpoint&& other) noexcept;
    UnixSocketEndpoint& operator=(UnixSocketEndpoint&& other) = delete;
    UnixSocketEndpoint(const UnixSocketEndpoint&) = delete;
    UnixSocketEndpoint& operator=(const UnixSocketEndpoint&) = delete;

    Acceptor& acceptor();

private:
    explicit UnixSocketEndpoint(boost::asio::io_context& ioc);
    void cleanup() noexcept;

    Acceptor acceptor_;
    int parent_fd_ { -1 };
    std::string filename_;
    dev_t socket_device_ { };
    ino_t socket_inode_ { };
    bool owns_socket_ { false };
};
