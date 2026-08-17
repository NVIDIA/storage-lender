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

#include <cstdint>
#include <string_view>

enum class LogLevel : uint8_t {
    TRACE,
    DEBUG,
    INFO,
    WARN,
    ERROR,
    CRITICAL,
    OFF,
};

struct LoggingConfig {
    LogLevel level { LogLevel::INFO };

    bool operator==(const LoggingConfig&) const = default;
};

constexpr std::string_view log_level_name(LogLevel level)
{
    switch (level) {
    case LogLevel::TRACE:
        return "trace";
    case LogLevel::DEBUG:
        return "debug";
    case LogLevel::INFO:
        return "info";
    case LogLevel::WARN:
        return "warn";
    case LogLevel::ERROR:
        return "error";
    case LogLevel::CRITICAL:
        return "critical";
    case LogLevel::OFF:
        return "off";
    }
    return "unknown";
}

constexpr bool log_level_enabled(LogLevel threshold, LogLevel event)
{
    return threshold != LogLevel::OFF && event >= threshold;
}
