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

#include "socket_config.hpp"

#include <sys/stat.h>
#include <sys/un.h>

std::optional<std::string> socket_path_validation_error(std::string_view path)
{
    if (path.empty()) {
        return "path must not be empty";
    }
    if (path.front() != '/') {
        return "path must be absolute";
    }
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        return "path is too long for a Unix socket";
    }
    if (path.size() == 1 || path.back() == '/') {
        return "path must end with a socket filename";
    }

    for (std::size_t begin = 1; begin < path.size();) {
        const auto end = path.find('/', begin);
        const auto component = path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin);
        if (component == "." || component == "..") {
            return "path must not contain . or .. components";
        }
        begin = end == std::string_view::npos ? path.size() : end + 1;
    }
    return std::nullopt;
}

bool is_allowed_socket_mode(mode_t mode)
{
    constexpr mode_t REQUIRED = S_IRUSR | S_IWUSR;
    constexpr mode_t ALLOWED = REQUIRED | S_IRGRP | S_IWGRP;
    return (mode & REQUIRED) == REQUIRED && (mode & ~ALLOWED) == 0;
}
