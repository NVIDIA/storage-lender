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

#include "logging_config.hpp"

#ifndef SPDLOG_ACTIVE_LEVEL
#error "SPDLOG_ACTIVE_LEVEL must be defined by the server target"
#endif

#include <spdlog/spdlog.h>

#include <cstdlib>

#define log_trace(...) SPDLOG_TRACE(__VA_ARGS__)
#define log_debug(...) SPDLOG_DEBUG(__VA_ARGS__)
#define log_info(...) SPDLOG_INFO(__VA_ARGS__)
#define log_warn(...) SPDLOG_WARN(__VA_ARGS__)
#define log_error(...) SPDLOG_ERROR(__VA_ARGS__)

#define fail(...)                                                                                                      \
    do {                                                                                                               \
        SPDLOG_CRITICAL(__VA_ARGS__);                                                                                  \
        std::abort();                                                                                                  \
    } while (false)

namespace logging {

LogLevel compiled_minimum_level();
LogLevel default_level();
bool is_available(LogLevel level);
void initialize();
void set_level(LogLevel level);
LogLevel active_level();

}
