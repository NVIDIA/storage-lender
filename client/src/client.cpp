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

#include "storage_lender/client.hpp"
#include "client.pb.h"
#include "storage_lender/wire/framing.hpp"
#include "wire.pb.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace storage_lender {

namespace {

    using ClientMethod = nvidia::storage_lender::v1::MethodId;
    using Request = nvidia::storage_lender::wire::v1::Request;
    using Response = nvidia::storage_lender::wire::v1::Response;
    using StatusCode = nvidia::storage_lender::wire::v1::StatusCode;

    ClientError status_to_client_error(int32_t code)
    {
        switch (static_cast<StatusCode>(code)) {
        case StatusCode::NOT_FOUND:
            return ClientError::NOT_FOUND;
        case StatusCode::INVALID_ARGUMENT:
            return ClientError::INVALID_ARGUMENT;
        case StatusCode::PERMISSION_DENIED:
            return ClientError::PERMISSION_DENIED;
        case StatusCode::RESOURCE_EXHAUSTED:
            return ClientError::RESOURCE_EXHAUSTED;
        case StatusCode::DEADLINE_EXCEEDED:
            return ClientError::DEADLINE_EXCEEDED;
        case StatusCode::FAILED_PRECONDITION:
            return ClientError::FAILED_PRECONDITION;
        default:
            return ClientError::INTERNAL;
        }
    }

} // namespace

ClientError StorageLenderClient::connect(const std::string& socket_path, StorageLenderClient& out)
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return ClientError::IO_ERROR;
    }

    sockaddr_un addr { };
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return ClientError::IO_ERROR;
    }

    out = StorageLenderClient(fd);
    return ClientError::OK;
}

StorageLenderClient::StorageLenderClient() noexcept
    : sock_fd_(-1)
{
}

StorageLenderClient::StorageLenderClient(int sock_fd)
    : sock_fd_(sock_fd)
{
}

StorageLenderClient::StorageLenderClient(StorageLenderClient&& other) noexcept
    : sock_fd_(std::exchange(other.sock_fd_, -1))
{
}

StorageLenderClient& StorageLenderClient::operator=(StorageLenderClient&& other) noexcept
{
    if (this != &other) {
        if (sock_fd_ >= 0) {
            ::close(sock_fd_);
        }
        sock_fd_ = std::exchange(other.sock_fd_, -1);
    }
    return *this;
}

StorageLenderClient::~StorageLenderClient()
{
    if (sock_fd_ >= 0) {
        ::close(sock_fd_);
    }
}

bool StorageLenderClient::write_exact(const void* buf, size_t n)
{
    size_t total = 0;
    while (total < n) {
        ssize_t r = ::send(sock_fd_, static_cast<const char*>(buf) + total, n - total, MSG_NOSIGNAL);
        if (r <= 0) {
            return false;
        }
        total += static_cast<size_t>(r);
    }
    return true;
}

bool StorageLenderClient::read_exact(void* buf, size_t n)
{
    size_t total = 0;
    while (total < n) {
        ssize_t r = ::read(sock_fd_, static_cast<char*>(buf) + total, n - total);
        if (r <= 0) {
            return false;
        }
        total += static_cast<size_t>(r);
    }
    return true;
}

bool StorageLenderClient::send_frame(const std::string& data)
{
    wire::FrameHeader header { };
    if (!wire::encode_frame_header(data.size(), header)) {
        return false;
    }
    if (!write_exact(header.data(), header.size())) {
        return false;
    }
    return write_exact(data.data(), data.size());
}

bool StorageLenderClient::recv_frame(std::string& data)
{
    wire::FrameHeader header { };
    if (!read_exact(header.data(), header.size())) {
        return false;
    }
    uint32_t len = 0;
    if (!wire::decode_frame_header(header, len)) {
        return false;
    }
    data.resize(len);
    return read_exact(data.data(), len);
}

ClientError StorageLenderClient::rpc(
    uint32_t method, int32_t& status_code, std::string& response_payload, const std::string& request)
{
    Request req;
    req.set_method(method);
    if (!request.empty()) {
        req.set_payload(request);
    }

    if (!send_frame(req.SerializeAsString())) {
        return ClientError::IO_ERROR;
    }

    std::string raw;
    if (!recv_frame(raw)) {
        return ClientError::IO_ERROR;
    }

    Response resp;
    if (!resp.ParseFromString(raw)) {
        return ClientError::PROTOCOL_ERROR;
    }

    status_code = static_cast<int32_t>(resp.status_code());
    response_payload = resp.payload();
    return ClientError::OK;
}

ClientError StorageLenderClient::transfer_fd(int fd, uint32_t& fd_id)
{
    Request req;
    req.set_method(static_cast<uint32_t>(ClientMethod::TRANSFER_FD));
    if (!send_frame(req.SerializeAsString())) {
        return ClientError::IO_ERROR;
    }

    char dummy = 'x';
    iovec iov { &dummy, 1 };
    alignas(cmsghdr) char cmsg_buf[CMSG_SPACE(sizeof(int))];
    msghdr msg { };
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);
    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    if (::sendmsg(sock_fd_, &msg, MSG_NOSIGNAL) < 0) {
        return ClientError::IO_ERROR;
    }

    std::string raw;
    if (!recv_frame(raw)) {
        return ClientError::IO_ERROR;
    }

    Response resp;
    if (!resp.ParseFromString(raw)) {
        return ClientError::PROTOCOL_ERROR;
    }
    if (resp.status_code() != StatusCode::OK) {
        return status_to_client_error(static_cast<int32_t>(resp.status_code()));
    }

    nvidia::storage_lender::v1::TransferFdResponse body;
    if (!body.ParseFromString(resp.payload())) {
        return ClientError::PROTOCOL_ERROR;
    }

    fd_id = body.fd_id();
    return ClientError::OK;
}

ClientError StorageLenderClient::map_buffer(uint32_t fd_id, uint64_t size, uint64_t& iova, uint32_t alignment)
{
    nvidia::storage_lender::v1::MapBufferRequest body;
    body.set_fd_id(fd_id);
    body.set_size(size);
    body.set_alignment(alignment);

    int32_t status;
    std::string payload;
    if (auto err = rpc(static_cast<uint32_t>(ClientMethod::MAP_BUFFER), status, payload, body.SerializeAsString());
        err != ClientError::OK) {
        return err;
    }
    if (status != 0) {
        return status_to_client_error(status);
    }

    nvidia::storage_lender::v1::MapBufferResponse resp;
    if (!resp.ParseFromString(payload)) {
        return ClientError::PROTOCOL_ERROR;
    }

    iova = resp.iova();
    return ClientError::OK;
}

ClientError StorageLenderClient::unmap_buffer(uint64_t iova)
{
    nvidia::storage_lender::v1::UnmapBufferRequest body;
    body.set_iova(iova);

    int32_t status;
    std::string payload;
    if (auto err = rpc(static_cast<uint32_t>(ClientMethod::UNMAP_BUFFER), status, payload, body.SerializeAsString());
        err != ClientError::OK) {
        return err;
    }
    if (status != 0) {
        return status_to_client_error(status);
    }

    return ClientError::OK;
}

ClientError StorageLenderClient::open_device(const std::string& pci_address, OpenDeviceMode mode, uint32_t& device_id)
{
    nvidia::storage_lender::v1::OpenDeviceRequest body;
    body.set_pci_address(pci_address);
    body.set_open_mode(
        mode == OpenDeviceMode::EXCLUSIVE ? nvidia::storage_lender::v1::EXCLUSIVE : nvidia::storage_lender::v1::SHARED);

    int32_t status;
    std::string payload;
    if (auto err = rpc(static_cast<uint32_t>(ClientMethod::OPEN_DEVICE), status, payload, body.SerializeAsString());
        err != ClientError::OK) {
        return err;
    }
    if (status != 0) {
        return status_to_client_error(status);
    }

    nvidia::storage_lender::v1::OpenDeviceResponse resp;
    if (!resp.ParseFromString(payload)) {
        return ClientError::PROTOCOL_ERROR;
    }

    device_id = resp.device_id();
    return ClientError::OK;
}

ClientError StorageLenderClient::close_device(uint32_t device_id)
{
    nvidia::storage_lender::v1::CloseDeviceRequest body;
    body.set_device_id(device_id);

    int32_t status;
    std::string payload;
    if (auto err = rpc(static_cast<uint32_t>(ClientMethod::CLOSE_DEVICE), status, payload, body.SerializeAsString());
        err != ClientError::OK) {
        return err;
    }
    if (status != 0) {
        return status_to_client_error(status);
    }

    return ClientError::OK;
}

ClientError StorageLenderClient::get_device_info(uint32_t device_id, DeviceInfo& info)
{
    nvidia::storage_lender::v1::DeviceInfoRequest body;
    body.set_device_id(device_id);

    int32_t status;
    std::string payload;
    if (auto err = rpc(static_cast<uint32_t>(ClientMethod::GET_DEVICE_INFO), status, payload, body.SerializeAsString());
        err != ClientError::OK) {
        return err;
    }
    if (status != 0) {
        return status_to_client_error(status);
    }

    nvidia::storage_lender::v1::DeviceInfoResponse resp;
    if (!resp.ParseFromString(payload)) {
        return ClientError::PROTOCOL_ERROR;
    }

    info.model = resp.model();
    info.pci_resource_path = resp.pci_resource_path();
    info.max_queue_entries = resp.max_queue_entries();
    info.page_size = resp.page_size();
    info.num_io_queues = resp.num_io_queues();
    std::transform(resp.namespaces().begin(), resp.namespaces().end(), std::back_inserter(info.namespaces),
        [](const auto& ns) -> NamespaceInfo { return { ns.ns_id(), ns.block_size(), ns.block_count() }; });

    return ClientError::OK;
}

ClientError StorageLenderClient::create_cq(uint32_t device_id, uint64_t iova, uint32_t queue_size, QueueInfo& info)
{
    nvidia::storage_lender::v1::CreateCompletionQueueRequest body;
    body.set_device_id(device_id);
    body.set_iova(iova);
    body.set_queue_size(queue_size);

    int32_t status;
    std::string payload;
    if (auto err
        = rpc(static_cast<uint32_t>(ClientMethod::CREATE_COMPLETION_QUEUE), status, payload, body.SerializeAsString());
        err != ClientError::OK) {
        return err;
    }
    if (status != 0) {
        return status_to_client_error(status);
    }

    nvidia::storage_lender::v1::CreateCompletionQueueResponse resp;
    if (!resp.ParseFromString(payload)) {
        return ClientError::PROTOCOL_ERROR;
    }

    info = QueueInfo { resp.cq_id(), resp.cq_db_offset() };
    return ClientError::OK;
}

ClientError StorageLenderClient::delete_cq(uint32_t cq_id)
{
    nvidia::storage_lender::v1::DeleteCompletionQueueRequest body;
    body.set_cq_id(cq_id);

    int32_t status;
    std::string payload;
    if (auto err
        = rpc(static_cast<uint32_t>(ClientMethod::DELETE_COMPLETION_QUEUE), status, payload, body.SerializeAsString());
        err != ClientError::OK) {
        return err;
    }
    if (status != 0) {
        return status_to_client_error(status);
    }

    return ClientError::OK;
}

ClientError StorageLenderClient::create_sq(
    uint32_t device_id, uint32_t cq_id, uint64_t iova, uint32_t queue_size, QueueInfo& info)
{
    nvidia::storage_lender::v1::CreateSubmissionQueueRequest body;
    body.set_device_id(device_id);
    body.set_cq_id(cq_id);
    body.set_iova(iova);
    body.set_queue_size(queue_size);

    int32_t status;
    std::string payload;
    if (auto err
        = rpc(static_cast<uint32_t>(ClientMethod::CREATE_SUBMISSION_QUEUE), status, payload, body.SerializeAsString());
        err != ClientError::OK) {
        return err;
    }
    if (status != 0) {
        return status_to_client_error(status);
    }

    nvidia::storage_lender::v1::CreateSubmissionQueueResponse resp;
    if (!resp.ParseFromString(payload)) {
        return ClientError::PROTOCOL_ERROR;
    }

    info = QueueInfo { resp.sq_id(), resp.sq_db_offset() };
    return ClientError::OK;
}

ClientError StorageLenderClient::delete_sq(uint32_t sq_id)
{
    nvidia::storage_lender::v1::DeleteSubmissionQueueRequest body;
    body.set_sq_id(sq_id);

    int32_t status;
    std::string payload;
    if (auto err
        = rpc(static_cast<uint32_t>(ClientMethod::DELETE_SUBMISSION_QUEUE), status, payload, body.SerializeAsString());
        err != ClientError::OK) {
        return err;
    }
    if (status != 0) {
        return status_to_client_error(status);
    }

    return ClientError::OK;
}

} // namespace storage_lender
