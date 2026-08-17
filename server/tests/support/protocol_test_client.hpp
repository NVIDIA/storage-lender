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

#include "client.pb.h"
#include "protocol.hpp"
#include "wire.pb.h"

#include <google/protobuf/message_lite.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

using MethodId = nvidia::storage_lender::v1::MethodId;

class ProtocolTestClient {
public:
    using Response = nvidia::storage_lender::wire::v1::Response;

    static std::expected<ProtocolTestClient, int> connect(std::string_view socket_path);

    ProtocolTestClient(const ProtocolTestClient&) = delete;
    ProtocolTestClient& operator=(const ProtocolTestClient&) = delete;
    ProtocolTestClient(ProtocolTestClient&& other) noexcept;
    ProtocolTestClient& operator=(ProtocolTestClient&& other) noexcept;
    ~ProtocolTestClient();

    bool send_request(uint32_t method);
    bool send_request(uint32_t method, const google::protobuf::MessageLite& message);
    bool send_request_raw_payload(uint32_t method, std::string_view payload);
    bool send_raw_request_frame(std::string_view data);
    bool send_partial_frame(uint32_t declared_len, std::string_view partial);
    bool recv_response(Response& response);

    std::expected<Response, int> call(MethodId method);
    std::expected<Response, int> call(MethodId method, const google::protobuf::MessageLite& message);
    std::expected<uint32_t, int> transfer_fd(int fd);
    std::expected<uint64_t, int> map_buffer(uint32_t fd_id, uint64_t size);
    std::expected<uint32_t, int> open_shared_device(std::string_view pci_address);

    bool send_fd_scm_rights(int fd);
    bool send_byte_without_scm();
    bool send_two_fds_in_one_cmsg(int first, int second);
    bool send_three_fds_in_one_cmsg(int first, int second, int third);
    bool send_fd_with_extra_cmsg(int first, int second);
    bool send_oversized_header();

private:
    explicit ProtocolTestClient(int socket_fd);
    bool send_fds_in_one_cmsg(const int* fds, std::size_t count);
    bool send_frame(std::string_view data);
    bool read_exact(void* buffer, std::size_t size);
    bool write_exact(const void* buffer, std::size_t size);

    int socket_fd_ { -1 };
};
