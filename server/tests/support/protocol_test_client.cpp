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

#include "protocol_test_client.hpp"
#include "storage_lender/wire/framing.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

std::expected<ProtocolTestClient, int> ProtocolTestClient::connect(std::string_view socket_path)
{
    const auto socket_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return std::unexpected(errno);
    }

    ProtocolTestClient client { socket_fd };
    struct sockaddr_un address { };
    address.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(address.sun_path)) {
        return std::unexpected(ENAMETOOLONG);
    }
    ::memcpy(address.sun_path, socket_path.data(), socket_path.size());
    if (::connect(socket_fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
        const auto error = errno;
        return std::unexpected(error);
    }
    return client;
}

ProtocolTestClient::ProtocolTestClient(int socket_fd)
    : socket_fd_(socket_fd)
{
}

ProtocolTestClient::ProtocolTestClient(ProtocolTestClient&& other) noexcept
    : socket_fd_(std::exchange(other.socket_fd_, -1))
{
}

ProtocolTestClient& ProtocolTestClient::operator=(ProtocolTestClient&& other) noexcept
{
    if (this != &other) {
        if (socket_fd_ >= 0) {
            ::close(socket_fd_);
        }
        socket_fd_ = std::exchange(other.socket_fd_, -1);
    }
    return *this;
}

ProtocolTestClient::~ProtocolTestClient()
{
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
    }
}

bool ProtocolTestClient::send_request(uint32_t method)
{
    nvidia::storage_lender::wire::v1::Request request;
    request.set_method(method);
    return send_frame(request.SerializeAsString());
}

bool ProtocolTestClient::send_request(uint32_t method, const google::protobuf::MessageLite& message)
{
    nvidia::storage_lender::wire::v1::Request request;
    request.set_method(method);
    request.set_payload(message.SerializeAsString());
    return send_frame(request.SerializeAsString());
}

bool ProtocolTestClient::send_request_raw_payload(uint32_t method, std::string_view payload)
{
    nvidia::storage_lender::wire::v1::Request request;
    request.set_method(method);
    request.set_payload(payload.data(), payload.size());
    return send_frame(request.SerializeAsString());
}

bool ProtocolTestClient::send_raw_request_frame(std::string_view data) { return send_frame(data); }

bool ProtocolTestClient::recv_response(Response& response)
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

std::expected<ProtocolTestClient::Response, int> ProtocolTestClient::call(MethodId method)
{
    if (!send_request(static_cast<uint32_t>(method))) {
        return std::unexpected(EIO);
    }
    Response response;
    if (!recv_response(response)) {
        return std::unexpected(EIO);
    }
    return response;
}

std::expected<ProtocolTestClient::Response, int> ProtocolTestClient::call(
    MethodId method, const google::protobuf::MessageLite& message)
{
    if (!send_request(static_cast<uint32_t>(method), message)) {
        return std::unexpected(EIO);
    }
    Response response;
    if (!recv_response(response)) {
        return std::unexpected(EIO);
    }
    return response;
}

std::expected<uint32_t, int> ProtocolTestClient::transfer_fd(int fd)
{
    if (!send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD)) || !send_fd_scm_rights(fd)) {
        return std::unexpected(EIO);
    }
    Response response;
    if (!recv_response(response)) {
        return std::unexpected(EIO);
    }
    nvidia::storage_lender::v1::TransferFdResponse payload;
    if (response.status_code() != static_cast<int32_t>(StatusCode::OK)
        || !payload.ParseFromString(response.payload())) {
        return std::unexpected(EPROTO);
    }
    return payload.fd_id();
}

std::expected<uint64_t, int> ProtocolTestClient::map_buffer(uint32_t fd_id, uint64_t size)
{
    nvidia::storage_lender::v1::MapBufferRequest request;
    request.set_fd_id(fd_id);
    request.set_size(size);
    auto response = call(MethodId::MAP_BUFFER, request);
    if (!response) {
        return std::unexpected(response.error());
    }
    if (response->status_code() != static_cast<int32_t>(StatusCode::OK)) {
        return std::unexpected(EPROTO);
    }
    nvidia::storage_lender::v1::MapBufferResponse payload;
    if (!payload.ParseFromString(response->payload())) {
        return std::unexpected(EPROTO);
    }
    return payload.iova();
}

std::expected<uint32_t, int> ProtocolTestClient::open_shared_device(std::string_view pci_address)
{
    nvidia::storage_lender::v1::OpenDeviceRequest request;
    request.set_pci_address(std::string { pci_address });
    request.set_open_mode(nvidia::storage_lender::v1::SHARED);
    auto response = call(MethodId::OPEN_DEVICE, request);
    if (!response) {
        return std::unexpected(response.error());
    }
    if (response->status_code() != static_cast<int32_t>(StatusCode::OK)) {
        return std::unexpected(EPROTO);
    }
    nvidia::storage_lender::v1::OpenDeviceResponse payload;
    if (!payload.ParseFromString(response->payload())) {
        return std::unexpected(EPROTO);
    }
    return payload.device_id();
}

bool ProtocolTestClient::send_fd_scm_rights(int fd)
{
    char dummy = 'x';
    struct iovec iov { .iov_base = &dummy, .iov_len = 1 };
    alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int))] { };
    struct msghdr message { };
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    auto* cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    ::memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    return ::sendmsg(socket_fd_, &message, 0) >= 0;
}

bool ProtocolTestClient::send_byte_without_scm()
{
    char dummy = 'x';
    return ::write(socket_fd_, &dummy, 1) == 1;
}

bool ProtocolTestClient::send_two_fds_in_one_cmsg(int first, int second)
{
    const int fds[] { first, second };
    return send_fds_in_one_cmsg(fds, std::size(fds));
}

bool ProtocolTestClient::send_three_fds_in_one_cmsg(int first, int second, int third)
{
    const int fds[] { first, second, third };
    return send_fds_in_one_cmsg(fds, std::size(fds));
}

bool ProtocolTestClient::send_fd_with_extra_cmsg(int first, int second)
{
    char dummy = 'x';
    struct iovec iov { .iov_base = &dummy, .iov_len = 1 };
    alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(int))] { };
    struct msghdr message { };
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    auto* cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    ::memcpy(CMSG_DATA(cmsg), &first, sizeof(first));
    cmsg = CMSG_NXTHDR(&message, cmsg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    ::memcpy(CMSG_DATA(cmsg), &second, sizeof(second));
    return ::sendmsg(socket_fd_, &message, 0) >= 0;
}

bool ProtocolTestClient::send_oversized_header()
{
    const auto length = storage_lender::wire::MAX_FRAME_BYTES + 1;
    storage_lender::wire::FrameHeader header { };
    for (std::size_t index = 0; index < header.size(); ++index) {
        header[index] = static_cast<std::byte>((length >> (index * 8U)) & 0xffU);
    }
    return write_exact(header.data(), header.size());
}

bool ProtocolTestClient::send_partial_frame(uint32_t declared_len, std::string_view partial)
{
    storage_lender::wire::FrameHeader header { };
    if (!storage_lender::wire::encode_frame_header(declared_len, header)) {
        return false;
    }
    if (!write_exact(header.data(), header.size())) {
        return false;
    }
    return write_exact(partial.data(), partial.size());
}

bool ProtocolTestClient::send_fds_in_one_cmsg(const int* fds, std::size_t count)
{
    char dummy = 'x';
    struct iovec iov { .iov_base = &dummy, .iov_len = 1 };
    std::vector<char> control(CMSG_SPACE(count * sizeof(int)));
    struct msghdr message { };
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    auto* cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(count * sizeof(int));
    ::memcpy(CMSG_DATA(cmsg), fds, count * sizeof(int));
    return ::sendmsg(socket_fd_, &message, 0) >= 0;
}

bool ProtocolTestClient::send_frame(std::string_view data)
{
    storage_lender::wire::FrameHeader header { };
    if (!storage_lender::wire::encode_frame_header(data.size(), header)) {
        return false;
    }
    if (!write_exact(header.data(), header.size())) {
        return false;
    }
    return write_exact(data.data(), data.size());
}

bool ProtocolTestClient::read_exact(void* buffer, std::size_t size)
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

bool ProtocolTestClient::write_exact(const void* buffer, std::size_t size)
{
    std::size_t total = 0;
    while (total < size) {
        const auto count = ::write(socket_fd_, static_cast<const char*>(buffer) + total, size - total);
        if (count <= 0) {
            return false;
        }
        total += static_cast<std::size_t>(count);
    }
    return true;
}
