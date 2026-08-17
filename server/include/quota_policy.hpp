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
#include <optional>
#include <string>
#include <unordered_map>

using PrincipalId = std::string;
using QuotaLimit = std::optional<uint64_t>;

enum class QuotaMode { ENFORCED, UNLIMITED };

struct QuotaLimits {
    QuotaLimit sessions;
    QuotaLimit transferred_fds;
    QuotaLimit mapped_buffers;
    QuotaLimit max_buffer_bytes;
    QuotaLimit mapped_bytes;
    QuotaLimit device_handles;
    QuotaLimit completion_queues;
    QuotaLimit submission_queues;

    bool operator==(const QuotaLimits&) const = default;
};

struct QuotaPolicy {
    QuotaMode mode { QuotaMode::UNLIMITED };
    QuotaLimits global;
    QuotaLimits default_principal;
    std::unordered_map<PrincipalId, QuotaLimits> principals;

    const QuotaLimits& effective_limits(const PrincipalId& principal) const
    {
        auto it = principals.find(principal);
        return it == principals.end() ? default_principal : it->second;
    }
};
