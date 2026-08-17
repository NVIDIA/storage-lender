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

#include "logging.hpp"

#include <spdlog/sinks/systemd_sink.h>

#include <cstdlib>
#include <memory>

namespace {

spdlog::level::level_enum to_spdlog_level(LogLevel level)
{
    switch (level) {
    case LogLevel::TRACE:
        return spdlog::level::trace;
    case LogLevel::DEBUG:
        return spdlog::level::debug;
    case LogLevel::INFO:
        return spdlog::level::info;
    case LogLevel::WARN:
        return spdlog::level::warn;
    case LogLevel::ERROR:
        return spdlog::level::err;
    case LogLevel::CRITICAL:
        return spdlog::level::critical;
    case LogLevel::OFF:
        return spdlog::level::off;
    }
    return spdlog::level::off;
}

LogLevel from_spdlog_level(spdlog::level::level_enum level)
{
    switch (level) {
    case spdlog::level::trace:
        return LogLevel::TRACE;
    case spdlog::level::debug:
        return LogLevel::DEBUG;
    case spdlog::level::info:
        return LogLevel::INFO;
    case spdlog::level::warn:
        return LogLevel::WARN;
    case spdlog::level::err:
        return LogLevel::ERROR;
    case spdlog::level::critical:
        return LogLevel::CRITICAL;
    case spdlog::level::off:
    case spdlog::level::n_levels:
        return LogLevel::OFF;
    }
    return LogLevel::OFF;
}

}

namespace logging {

LogLevel compiled_minimum_level()
{
#if SPDLOG_ACTIVE_LEVEL == SPDLOG_LEVEL_TRACE
    return LogLevel::TRACE;
#elif SPDLOG_ACTIVE_LEVEL == SPDLOG_LEVEL_DEBUG
    return LogLevel::DEBUG;
#else
#error "Storage Lender supports only trace or debug compiled logging floors"
#endif
}

LogLevel default_level() { return compiled_minimum_level() == LogLevel::TRACE ? LogLevel::TRACE : LogLevel::INFO; }

bool is_available(LogLevel level) { return level >= compiled_minimum_level(); }

void initialize()
{
    if (::getenv("JOURNAL_STREAM") != nullptr) {
        auto sink = std::make_shared<spdlog::sinks::systemd_sink_mt>();
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("", sink));
    } else {
        spdlog::set_pattern("%Y-%m-%d %H:%M:%S.%e%z %P/%t %L %s:%# %v");
    }
    set_level(default_level());
}

void set_level(LogLevel level) { spdlog::set_level(to_spdlog_level(level)); }

LogLevel active_level() { return from_spdlog_level(spdlog::get_level()); }

}
