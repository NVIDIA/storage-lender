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

#include "quota_manager.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <vector>

namespace {

struct Dimension {
    uint64_t QuotaAmounts::* amount;
    QuotaLimit QuotaLimits::* limit;
    QuotaResource resource;
};

constexpr std::array DIMENSIONS {
    Dimension { &QuotaAmounts::sessions, &QuotaLimits::sessions, QuotaResource::SESSIONS },
    Dimension { &QuotaAmounts::transferred_fds, &QuotaLimits::transferred_fds, QuotaResource::TRANSFERRED_FDS },
    Dimension { &QuotaAmounts::mapped_buffers, &QuotaLimits::mapped_buffers, QuotaResource::MAPPED_BUFFERS },
    Dimension { &QuotaAmounts::mapped_bytes, &QuotaLimits::mapped_bytes, QuotaResource::MAPPED_BYTES },
    Dimension { &QuotaAmounts::device_handles, &QuotaLimits::device_handles, QuotaResource::DEVICE_HANDLES },
    Dimension { &QuotaAmounts::completion_queues, &QuotaLimits::completion_queues, QuotaResource::COMPLETION_QUEUES },
    Dimension { &QuotaAmounts::submission_queues, &QuotaLimits::submission_queues, QuotaResource::SUBMISSION_QUEUES },
};

bool is_zero(const QuotaAmounts& amounts) { return amounts == QuotaAmounts { }; }

bool is_zero(const UsageTotals& usage) { return is_zero(usage.active) && is_zero(usage.orphan); }

uint64_t current(const UsageTotals& usage, uint64_t QuotaAmounts::* member)
{
    return usage.active.*member + usage.orphan.*member;
}

bool exceeds_limit(uint64_t current, uint64_t requested, uint64_t limit)
{
    return requested > limit || current > limit - requested;
}

bool addition_overflows(uint64_t current, uint64_t requested)
{
    return current > std::numeric_limits<uint64_t>::max() - requested;
}

void add(QuotaAmounts& destination, const QuotaAmounts& amounts)
{
    for (const auto& dimension : DIMENSIONS) {
        destination.*dimension.amount += amounts.*dimension.amount;
    }
}

void subtract(QuotaAmounts& destination, const QuotaAmounts& amounts)
{
    for (const auto& dimension : DIMENSIONS) {
        destination.*dimension.amount -= amounts.*dimension.amount;
    }
}

} // namespace

QuotaLease::QuotaLease(QuotaManager& manager, PrincipalId principal, QuotaAmounts amounts)
    : manager_ { &manager }
    , principal_ { std::move(principal) }
    , amounts_ { amounts }
{
}

QuotaLease::~QuotaLease() { release(); }

QuotaLease::QuotaLease(QuotaLease&& other) noexcept
    : manager_ { std::exchange(other.manager_, nullptr) }
    , principal_ { std::move(other.principal_) }
    , amounts_ { std::exchange(other.amounts_, { }) }
{
}

QuotaLease& QuotaLease::operator=(QuotaLease&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    release();
    manager_ = std::exchange(other.manager_, nullptr);
    principal_ = std::move(other.principal_);
    amounts_ = std::exchange(other.amounts_, { });
    return *this;
}

void QuotaLease::mark_orphaned()
{
    if (manager_ == nullptr) {
        return;
    }
    manager_->orphan(principal_, amounts_);
    manager_ = nullptr;
}

const PrincipalId& QuotaLease::principal() const { return principal_; }

const QuotaAmounts& QuotaLease::amounts() const { return amounts_; }

void QuotaLease::release()
{
    if (manager_ == nullptr) {
        return;
    }
    manager_->release(principal_, amounts_);
    manager_ = nullptr;
}

QuotaManager::QuotaManager(QuotaPolicy policy)
    : policy_ { std::move(policy) }
{
}

std::expected<QuotaLease, QuotaAcquireError> QuotaManager::acquire(const PrincipalId& principal, QuotaAmounts amounts)
{
    const auto& principal_limits = policy_.effective_limits(principal);
    const auto usage_it = principal_usage_.find(principal);
    const auto empty_usage = UsageTotals { };
    const auto& existing_principal_usage = usage_it == principal_usage_.end() ? empty_usage : usage_it->second;

    for (const auto& dimension : DIMENSIONS) {
        const auto requested = amounts.*dimension.amount;
        if (requested == 0) {
            continue;
        }

        const auto principal_current = current(existing_principal_usage, dimension.amount);
        const auto global_current = current(global_usage_, dimension.amount);
        const auto& principal_limit = principal_limits.*dimension.limit;
        const auto& global_limit = policy_.global.*dimension.limit;

        if (policy_.mode == QuotaMode::ENFORCED && principal_limit.has_value()
            && exceeds_limit(principal_current, requested, *principal_limit)) {
            return std::unexpected(QuotaAcquireError { QuotaExceeded {
                .scope = QuotaScope::PRINCIPAL,
                .resource = dimension.resource,
                .principal = principal,
                .requested = requested,
                .principal_current = principal_current,
                .principal_limit = principal_limit,
                .global_current = global_current,
                .global_limit = global_limit,
            } });
        }
        if (addition_overflows(principal_current, requested)) {
            return std::unexpected(QuotaAcquireError { QuotaAccountingOverflow {
                .scope = QuotaScope::PRINCIPAL,
                .resource = dimension.resource,
                .principal = principal,
                .requested = requested,
                .current = principal_current,
            } });
        }
        if (policy_.mode == QuotaMode::ENFORCED && global_limit.has_value()
            && exceeds_limit(global_current, requested, *global_limit)) {
            return std::unexpected(QuotaAcquireError { QuotaExceeded {
                .scope = QuotaScope::GLOBAL,
                .resource = dimension.resource,
                .principal = principal,
                .requested = requested,
                .principal_current = principal_current,
                .principal_limit = principal_limit,
                .global_current = global_current,
                .global_limit = global_limit,
            } });
        }
        if (addition_overflows(global_current, requested)) {
            return std::unexpected(QuotaAcquireError { QuotaAccountingOverflow {
                .scope = QuotaScope::GLOBAL,
                .resource = dimension.resource,
                .principal = principal,
                .requested = requested,
                .current = global_current,
            } });
        }
    }

    auto& principal_usage = principal_usage_[principal];
    add(principal_usage.active, amounts);
    add(global_usage_.active, amounts);
    return QuotaLease { *this, principal, amounts };
}

std::expected<QuotaLease, QuotaAcquireError> QuotaManager::acquire_buffer(const PrincipalId& principal, uint64_t size)
{
    const auto& principal_limits = policy_.effective_limits(principal);
    if (policy_.mode == QuotaMode::ENFORCED) {
        if (principal_limits.max_buffer_bytes.has_value() && size > *principal_limits.max_buffer_bytes) {
            return std::unexpected(QuotaAcquireError { QuotaExceeded {
                .scope = QuotaScope::PRINCIPAL,
                .resource = QuotaResource::MAX_BUFFER_BYTES,
                .principal = principal,
                .requested = size,
                .principal_current = std::nullopt,
                .principal_limit = principal_limits.max_buffer_bytes,
                .global_current = std::nullopt,
                .global_limit = policy_.global.max_buffer_bytes,
            } });
        }
        if (policy_.global.max_buffer_bytes.has_value() && size > *policy_.global.max_buffer_bytes) {
            return std::unexpected(QuotaAcquireError { QuotaExceeded {
                .scope = QuotaScope::GLOBAL,
                .resource = QuotaResource::MAX_BUFFER_BYTES,
                .principal = principal,
                .requested = size,
                .principal_current = std::nullopt,
                .principal_limit = principal_limits.max_buffer_bytes,
                .global_current = std::nullopt,
                .global_limit = policy_.global.max_buffer_bytes,
            } });
        }
    }

    return acquire(principal, QuotaAmounts { .mapped_buffers = 1, .mapped_bytes = size });
}

std::expected<void, PolicyReloadError> QuotaManager::replace_policy(QuotaPolicy policy)
{
    std::vector<PrincipalId> removed_principals;
    for (const auto& [principal, limits] : policy_.principals) {
        static_cast<void>(limits);
        if (!policy.principals.contains(principal)) {
            removed_principals.push_back(principal);
        }
    }
    std::ranges::sort(removed_principals);

    for (const auto& principal : removed_principals) {
        const auto usage_it = principal_usage_.find(principal);
        if (usage_it != principal_usage_.end() && !is_zero(usage_it->second)) {
            return std::unexpected(PolicyReloadError {
                .principal = principal,
                .message = "cannot remove principal with active or orphan quota usage",
            });
        }
    }

    policy_ = std::move(policy);
    ++generation_;
    return { };
}

UsageSnapshot QuotaManager::snapshot() const
{
    return UsageSnapshot {
        .generation = generation_,
        .mode = policy_.mode,
        .global_usage = global_usage_,
        .principal_usage = principal_usage_,
        .global_limits = policy_.global,
        .default_limits = policy_.default_principal,
        .named_limits = policy_.principals,
    };
}

void QuotaManager::release(const PrincipalId& principal, const QuotaAmounts& amounts)
{
    auto usage_it = principal_usage_.find(principal);
    if (usage_it == principal_usage_.end()) {
        return;
    }

    subtract(usage_it->second.active, amounts);
    subtract(global_usage_.active, amounts);
    if (is_zero(usage_it->second)) {
        principal_usage_.erase(usage_it);
    }
}

void QuotaManager::orphan(const PrincipalId& principal, const QuotaAmounts& amounts)
{
    auto usage_it = principal_usage_.find(principal);
    if (usage_it == principal_usage_.end()) {
        return;
    }

    subtract(usage_it->second.active, amounts);
    add(usage_it->second.orphan, amounts);
    subtract(global_usage_.active, amounts);
    add(global_usage_.orphan, amounts);
    if (is_zero(usage_it->second)) {
        principal_usage_.erase(usage_it);
    }
}
