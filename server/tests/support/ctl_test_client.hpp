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

#include "ctl.pb.h"
#include "wire.pb.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

class CtlTestClient {
public:
    using Response = nvidia::storage_lender::wire::v1::Response;
    using QuotaState = nvidia::storage_lender::ctl::v1::GetQuotaStateResponse;
    using CommandLatency = nvidia::storage_lender::ctl::v1::GetCommandLatencyResponse;

    static std::expected<CtlTestClient, int> connect(std::string_view socket_path);

    CtlTestClient(const CtlTestClient&) = delete;
    CtlTestClient& operator=(const CtlTestClient&) = delete;
    CtlTestClient(CtlTestClient&& other) noexcept;
    CtlTestClient& operator=(CtlTestClient&& other) noexcept;
    ~CtlTestClient();

    bool send_request(uint32_t method, std::string_view payload = { });
    bool send_raw_request_frame(std::string_view data);
    bool recv_response(Response& response);
    std::expected<QuotaState, int> call_get_quota_state();
    std::expected<CommandLatency, int> call_get_command_latency();

private:
    explicit CtlTestClient(int socket_fd);
    bool send_frame(std::string_view data);
    bool read_exact(void* buffer, std::size_t size);
    bool write_exact(const void* buffer, std::size_t size);

    int socket_fd_ { -1 };
};
