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

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace storage_lender::ctl {

enum class OutputFormat {
    HUMAN,
    JSON,
};

enum class ColorMode {
    AUTO,
    ALWAYS,
    NEVER,
};

enum class CtlCommand {
    QUOTA,
    LATENCY,
};

constexpr std::string_view DEFAULT_CTL_SOCKET = "/run/storage-lender/ctl.sock";

struct CtlOptions {
    std::string socket_path { DEFAULT_CTL_SOCKET };
    OutputFormat format { OutputFormat::HUMAN };
    ColorMode color { ColorMode::AUTO };
    std::optional<CtlCommand> command;
    bool help { false };
    bool version { false };
};

std::expected<CtlOptions, std::string> parse_ctl_options(std::span<const std::string_view> arguments);

} // namespace storage_lender::ctl
