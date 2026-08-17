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

#include "ctl_test_client.hpp"

#include "storage_lender/wire/framing.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

std::expected<CtlTestClient, int> CtlTestClient::connect(std::string_view socket_path)
{
    const auto socket_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) {
        return std::unexpected(errno);
    }

    CtlTestClient client { socket_fd };
    struct sockaddr_un address { };
    address.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(address.sun_path)) {
        return std::unexpected(ENAMETOOLONG);
    }
    ::memcpy(address.sun_path, socket_path.data(), socket_path.size());
    if (::connect(socket_fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
        return std::unexpected(errno);
    }
    return client;
}

CtlTestClient::CtlTestClient(int socket_fd)
    : socket_fd_ { socket_fd }
{
}

CtlTestClient::CtlTestClient(CtlTestClient&& other) noexcept
    : socket_fd_ { std::exchange(other.socket_fd_, -1) }
{
}

CtlTestClient& CtlTestClient::operator=(CtlTestClient&& other) noexcept
{
    if (this != &other) {
        if (socket_fd_ >= 0) {
            ::close(socket_fd_);
        }
        socket_fd_ = std::exchange(other.socket_fd_, -1);
    }
    return *this;
}

CtlTestClient::~CtlTestClient()
{
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
    }
}

bool CtlTestClient::send_request(uint32_t method, std::string_view payload)
{
    nvidia::storage_lender::wire::v1::Request request;
    request.set_method(method);
    request.set_payload(payload.data(), payload.size());
    return send_frame(request.SerializeAsString());
}

bool CtlTestClient::send_raw_request_frame(std::string_view data) { return send_frame(data); }

bool CtlTestClient::recv_response(Response& response)
{
    storage_lender::wire::FrameHeader header { };
    if (!read_exact(header.data(), header.size())) {
        return false;
    }
    uint32_t length = 0;
    if (!storage_lender::wire::decode_frame_header(header, length)) {
        return false;
    }
    std::string payload(length, '\0');
    if (length > 0 && !read_exact(payload.data(), length)) {
        return false;
    }
    return response.ParseFromString(payload);
}

std::expected<CtlTestClient::QuotaState, int> CtlTestClient::call_get_quota_state()
{
    if (!send_request(static_cast<uint32_t>(nvidia::storage_lender::ctl::v1::GET_QUOTA_STATE))) {
        return std::unexpected(EIO);
    }
    Response response;
    if (!recv_response(response) || response.status_code() != nvidia::storage_lender::wire::v1::OK) {
        return std::unexpected(EPROTO);
    }
    QuotaState quota_state;
    if (!quota_state.ParseFromString(response.payload())) {
        return std::unexpected(EPROTO);
    }
    return quota_state;
}

std::expected<CtlTestClient::CommandLatency, int> CtlTestClient::call_get_command_latency()
{
    if (!send_request(static_cast<uint32_t>(nvidia::storage_lender::ctl::v1::GET_COMMAND_LATENCY))) {
        return std::unexpected(EIO);
    }
    Response response;
    if (!recv_response(response) || response.status_code() != nvidia::storage_lender::wire::v1::OK) {
        return std::unexpected(EPROTO);
    }
    CommandLatency command_latency;
    if (!command_latency.ParseFromString(response.payload())) {
        return std::unexpected(EPROTO);
    }
    return command_latency;
}

bool CtlTestClient::send_frame(std::string_view data)
{
    storage_lender::wire::FrameHeader header { };
    if (!storage_lender::wire::encode_frame_header(data.size(), header)) {
        return false;
    }
    return write_exact(header.data(), header.size()) && write_exact(data.data(), data.size());
}

bool CtlTestClient::read_exact(void* buffer, std::size_t size)
{
    std::size_t total = 0;
    while (total < size) {
        const auto count = ::read(socket_fd_, static_cast<char*>(buffer) + total, size - total);
        if (count <= 0) {
            return false;
        }
        total += static_cast<std::size_t>(count);
    }
    return true;
}

bool CtlTestClient::write_exact(const void* buffer, std::size_t size)
{
    std::size_t total = 0;
    while (total < size) {
        const auto count = ::send(socket_fd_, static_cast<const char*>(buffer) + total, size - total, MSG_NOSIGNAL);
        if (count <= 0) {
            return false;
        }
        total += static_cast<std::size_t>(count);
    }
    return true;
}
