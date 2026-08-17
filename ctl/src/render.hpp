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
#include "options.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace storage_lender::ctl {

bool color_enabled(ColorMode mode, bool stdout_is_tty, std::string_view term, bool no_color_set);
std::string render_human(const nvidia::storage_lender::ctl::v1::GetQuotaStateResponse& response, bool color);
std::expected<std::string, std::string> render_json(
    const nvidia::storage_lender::ctl::v1::GetQuotaStateResponse& response);
std::string render_human(const nvidia::storage_lender::ctl::v1::GetCommandLatencyResponse& response);
std::expected<std::string, std::string> render_json(
    const nvidia::storage_lender::ctl::v1::GetCommandLatencyResponse& response);

} // namespace storage_lender::ctl
