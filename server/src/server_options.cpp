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

#include "server_options.hpp"

std::expected<ServerOptions, std::string> parse_server_options(std::span<const std::string_view> arguments)
{
    if (arguments.empty()) {
        return ServerOptions { };
    }
    if (arguments.front() != "--config") {
        return std::unexpected("unknown argument: " + std::string { arguments.front() });
    }
    if (arguments.size() == 1) {
        return std::unexpected("--config requires a path");
    }
    if (arguments.size() != 2) {
        return std::unexpected("unexpected or repeated arguments after --config");
    }
    if (arguments[1].empty()) {
        return std::unexpected("--config path must not be empty");
    }
    return ServerOptions { .config_path = std::string { arguments[1] } };
}
