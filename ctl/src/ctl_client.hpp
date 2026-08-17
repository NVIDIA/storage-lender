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

#include <expected>
#include <string>
#include <string_view>

namespace storage_lender::ctl {

struct CtlClientError {
    enum Category {
        IO,
        SERVER_STATUS,
        PROTOCOL,
    };

    Category category;
    int system_error { 0 };
    nvidia::storage_lender::wire::v1::StatusCode server_status { nvidia::storage_lender::wire::v1::OK };
    std::string message;
};

std::expected<nvidia::storage_lender::ctl::v1::GetQuotaStateResponse, CtlClientError> get_quota_state(
    std::string_view socket_path);

std::expected<nvidia::storage_lender::ctl::v1::GetCommandLatencyResponse, CtlClientError> get_command_latency(
    std::string_view socket_path);

} // namespace storage_lender::ctl
