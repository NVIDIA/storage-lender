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

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

struct NvmeConnectOptions {
    uint32_t num_io_queues { };
    std::chrono::milliseconds admin_command_timeout { };

    friend bool operator==(const NvmeConnectOptions&, const NvmeConnectOptions&) = default;
};

struct DevicePolicy {
    NvmeConnectOptions default_options;
    std::unordered_map<std::string, NvmeConnectOptions> overrides;

    const NvmeConnectOptions& options_for(const std::string& canonical_bdf) const
    {
        const auto override = overrides.find(canonical_bdf);
        return override == overrides.end() ? default_options : override->second;
    }

    friend bool operator==(const DevicePolicy&, const DevicePolicy&) = default;
};
