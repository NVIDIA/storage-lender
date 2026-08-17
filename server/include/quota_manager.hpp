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

#include "quota_policy.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

struct QuotaAmounts {
    uint64_t sessions { };
    uint64_t transferred_fds { };
    uint64_t mapped_buffers { };
    uint64_t mapped_bytes { };
    uint64_t device_handles { };
    uint64_t completion_queues { };
    uint64_t submission_queues { };

    bool operator==(const QuotaAmounts&) const = default;
};

enum class QuotaResource {
    SESSIONS,
    TRANSFERRED_FDS,
    MAPPED_BUFFERS,
    MAX_BUFFER_BYTES,
    MAPPED_BYTES,
    DEVICE_HANDLES,
    COMPLETION_QUEUES,
    SUBMISSION_QUEUES,
};

enum class QuotaScope { PRINCIPAL, GLOBAL };

struct QuotaExceeded {
    QuotaScope scope;
    QuotaResource resource;
    PrincipalId principal;
    uint64_t requested;
    std::optional<uint64_t> principal_current;
    QuotaLimit principal_limit;
    std::optional<uint64_t> global_current;
    QuotaLimit global_limit;
};

struct QuotaAccountingOverflow {
    QuotaScope scope;
    QuotaResource resource;
    PrincipalId principal;
    uint64_t requested;
    uint64_t current;
};

using QuotaAcquireError = std::variant<QuotaExceeded, QuotaAccountingOverflow>;

struct PolicyReloadError {
    PrincipalId principal;
    std::string message;
};

struct UsageTotals {
    QuotaAmounts active;
    QuotaAmounts orphan;

    bool operator==(const UsageTotals&) const = default;
};

struct UsageSnapshot {
    uint64_t generation;
    QuotaMode mode;
    UsageTotals global_usage;
    std::unordered_map<PrincipalId, UsageTotals> principal_usage;
    QuotaLimits global_limits;
    QuotaLimits default_limits;
    std::unordered_map<PrincipalId, QuotaLimits> named_limits;
};

class QuotaManager;

class QuotaLease {
public:
    ~QuotaLease();
    QuotaLease(const QuotaLease&) = delete;
    QuotaLease& operator=(const QuotaLease&) = delete;
    QuotaLease(QuotaLease&& other) noexcept;
    QuotaLease& operator=(QuotaLease&& other) noexcept;

    void mark_orphaned();
    const PrincipalId& principal() const;
    const QuotaAmounts& amounts() const;

private:
    friend class QuotaManager;
    QuotaLease(QuotaManager& manager, PrincipalId principal, QuotaAmounts amounts);
    void release();

    QuotaManager* manager_;
    PrincipalId principal_;
    QuotaAmounts amounts_;
};

class QuotaManager {
public:
    explicit QuotaManager(QuotaPolicy policy);

    std::expected<QuotaLease, QuotaAcquireError> acquire(const PrincipalId& principal, QuotaAmounts amounts);
    std::expected<QuotaLease, QuotaAcquireError> acquire_buffer(const PrincipalId& principal, uint64_t size);
    std::expected<void, PolicyReloadError> replace_policy(QuotaPolicy policy);
    UsageSnapshot snapshot() const;

private:
    friend class QuotaLease;
    void release(const PrincipalId& principal, const QuotaAmounts& amounts);
    void orphan(const PrincipalId& principal, const QuotaAmounts& amounts);

    QuotaPolicy policy_;
    uint64_t generation_ { 1 };
    UsageTotals global_usage_;
    std::unordered_map<PrincipalId, UsageTotals> principal_usage_;
};
