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

#include "device_policy.hpp"
#include "logging_config.hpp"
#include "principal_resolver.hpp"
#include "quota_policy.hpp"
#include "socket_config.hpp"

#include <expected>
#include <string>
#include <string_view>

enum class ConfigErrorKind { CONFIG, FILE_NOT_FOUND, FILE_IO };

struct ConfigError {
    std::string source;
    std::string path;
    std::string message;
    ConfigErrorKind kind { ConfigErrorKind::CONFIG };
};

struct ServerConfig {
    SocketConfig api_socket;
    SocketConfig ctl_socket { };
    LoggingConfig logging { };
    DevicePolicy device_policy { };
    QuotaPolicy quota_policy;
    PrincipalResolver principal_resolver;
};

struct StartupConfig {
    ServerConfig config;
    bool used_builtin_defaults { };
};

class ConfigLoader {
public:
    static std::expected<ServerConfig, ConfigError> load(const std::string& path);
    static std::expected<StartupConfig, ConfigError> load_for_startup(const std::string& path);
    static std::expected<ServerConfig, ConfigError> parse(std::string_view document, std::string_view source_name);
};
