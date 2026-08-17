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

#include "ctl_client.hpp"

#include "storage_lender/wire/framing.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

namespace storage_lender::ctl {
namespace {

    using CtlMethod = nvidia::storage_lender::ctl::v1::MethodId;
    using Request = nvidia::storage_lender::wire::v1::Request;
    using Response = nvidia::storage_lender::wire::v1::Response;
    using StatusCode = nvidia::storage_lender::wire::v1::StatusCode;

    class Socket {
    public:
        explicit Socket(int socket_fd)
            : socket_fd_ { socket_fd }
        {
        }

        ~Socket()
        {
            if (socket_fd_ >= 0) {
                ::close(socket_fd_);
            }
        }

        int get() const { return socket_fd_; }

    private:
        int socket_fd_;
    };

    CtlClientError io_error(int system_error, std::string message)
    {
        return CtlClientError {
            .category = CtlClientError::IO,
            .system_error = system_error,
            .message = std::move(message),
        };
    }

    CtlClientError protocol_error(std::string message)
    {
        return CtlClientError {
            .category = CtlClientError::PROTOCOL,
            .message = std::move(message),
        };
    }

    bool write_exact(int socket_fd, const void* buffer, std::size_t size, int& system_error)
    {
        std::size_t total = 0;
        while (total < size) {
            const auto count = ::send(socket_fd, static_cast<const char*>(buffer) + total, size - total, MSG_NOSIGNAL);
            if (count <= 0) {
                system_error = count < 0 ? errno : 0;
                return false;
            }
            total += static_cast<std::size_t>(count);
        }
        return true;
    }

    bool read_exact(int socket_fd, void* buffer, std::size_t size, int& system_error)
    {
        std::size_t total = 0;
        while (total < size) {
            const auto count = ::read(socket_fd, static_cast<char*>(buffer) + total, size - total);
            if (count <= 0) {
                system_error = count < 0 ? errno : 0;
                return false;
            }
            total += static_cast<std::size_t>(count);
        }
        return true;
    }

    bool send_frame(int socket_fd, std::string_view payload, int& system_error)
    {
        storage_lender::wire::FrameHeader header { };
        if (!storage_lender::wire::encode_frame_header(payload.size(), header)) {
            return false;
        }
        return write_exact(socket_fd, header.data(), header.size(), system_error)
            && write_exact(socket_fd, payload.data(), payload.size(), system_error);
    }

    std::expected<std::string, CtlClientError> receive_frame(int socket_fd)
    {
        storage_lender::wire::FrameHeader header { };
        int system_error = 0;
        if (!read_exact(socket_fd, header.data(), header.size(), system_error)) {
            return std::unexpected(io_error(system_error, "failed to read response frame header"));
        }
        uint32_t payload_size = 0;
        if (!storage_lender::wire::decode_frame_header(header, payload_size)) {
            return std::unexpected(protocol_error("response frame exceeds the maximum size"));
        }

        std::string payload(payload_size, '\0');
        if (payload_size > 0 && !read_exact(socket_fd, payload.data(), payload.size(), system_error)) {
            return std::unexpected(io_error(system_error, "failed to read response frame payload"));
        }
        return payload;
    }

    std::expected<std::string, CtlClientError> call(std::string_view socket_path, CtlMethod method)
    {
        if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
            return std::unexpected(io_error(ENAMETOOLONG, "socket path exceeds sockaddr_un::sun_path"));
        }

        const auto socket_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (socket_fd < 0) {
            return std::unexpected(io_error(errno, "failed to create Unix socket"));
        }
        Socket socket { socket_fd };

        sockaddr_un address { };
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_path.data(), socket_path.size());
        if (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            return std::unexpected(io_error(errno, "failed to connect to ctl socket"));
        }

        Request request;
        request.set_method(static_cast<uint32_t>(method));
        int system_error = 0;
        if (!send_frame(socket.get(), request.SerializeAsString(), system_error)) {
            return std::unexpected(io_error(system_error, "failed to write ctl request"));
        }

        auto raw_response = receive_frame(socket.get());
        if (!raw_response) {
            return std::unexpected(raw_response.error());
        }

        Response response;
        if (!response.ParseFromString(*raw_response)) {
            return std::unexpected(protocol_error("failed to parse response envelope"));
        }
        if (response.status_code() != StatusCode::OK) {
            return std::unexpected(CtlClientError {
                .category = CtlClientError::SERVER_STATUS,
                .server_status = response.status_code(),
                .message = response.error_message(),
            });
        }

        return response.payload();
    }

} // anonymous namespace

std::expected<nvidia::storage_lender::ctl::v1::GetQuotaStateResponse, CtlClientError> get_quota_state(
    std::string_view socket_path)
{
    auto payload = call(socket_path, CtlMethod::GET_QUOTA_STATE);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    nvidia::storage_lender::ctl::v1::GetQuotaStateResponse quota_state;
    if (!quota_state.ParseFromString(*payload)) {
        return std::unexpected(protocol_error("failed to parse quota response"));
    }
    return quota_state;
}

std::expected<nvidia::storage_lender::ctl::v1::GetCommandLatencyResponse, CtlClientError> get_command_latency(
    std::string_view socket_path)
{
    auto payload = call(socket_path, CtlMethod::GET_COMMAND_LATENCY);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    nvidia::storage_lender::ctl::v1::GetCommandLatencyResponse response;
    if (!response.ParseFromString(*payload)) {
        return std::unexpected(protocol_error("failed to parse command latency response"));
    }
    return response;
}

} // namespace storage_lender::ctl
