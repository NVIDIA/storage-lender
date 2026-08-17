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

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace {

QuotaLimits finite_limits(uint64_t value)
{
    return QuotaLimits {
        .sessions = value,
        .transferred_fds = value,
        .mapped_buffers = value,
        .max_buffer_bytes = value,
        .mapped_bytes = value,
        .device_handles = value,
        .completion_queues = value,
        .submission_queues = value,
    };
}

QuotaPolicy enforced_policy(uint64_t principal_limit, uint64_t global_limit)
{
    return QuotaPolicy {
        .mode = QuotaMode::ENFORCED,
        .global = finite_limits(global_limit),
        .default_principal = finite_limits(principal_limit),
        .principals = { },
    };
}

bool is_zero(const QuotaAmounts& amounts) { return amounts == QuotaAmounts { }; }

const QuotaExceeded& quota_exceeded(const QuotaAcquireError& error) { return std::get<QuotaExceeded>(error); }

} // namespace

static_assert(!std::is_default_constructible_v<QuotaLease>);
static_assert(!std::is_copy_constructible_v<QuotaLease>);
static_assert(!std::is_copy_assignable_v<QuotaLease>);
static_assert(std::is_nothrow_move_constructible_v<QuotaLease>);
static_assert(std::is_nothrow_move_assignable_v<QuotaLease>);

TEST(QuotaManagerTest, ExactBoundarySucceedsForEveryAccumulatedDimension)
{
    constexpr auto AMOUNT = uint64_t { 7 };
    QuotaManager manager { enforced_policy(AMOUNT, AMOUNT) };

    auto result = manager.acquire("camera",
        QuotaAmounts {
            .sessions = AMOUNT,
            .transferred_fds = AMOUNT,
            .mapped_buffers = AMOUNT,
            .mapped_bytes = AMOUNT,
            .device_handles = AMOUNT,
            .completion_queues = AMOUNT,
            .submission_queues = AMOUNT,
        });

    ASSERT_TRUE(result.has_value());
    const auto snapshot = manager.snapshot();
    EXPECT_EQ(snapshot.global_usage.active, result->amounts());
    EXPECT_EQ(snapshot.principal_usage.at("camera").active, result->amounts());
}

TEST(QuotaManagerTest, OneUnitOverReportsPrincipalOrGlobalScope)
{
    struct Dimension {
        uint64_t QuotaAmounts::* amount;
        QuotaResource resource;
    };
    constexpr std::array DIMENSIONS {
        Dimension { &QuotaAmounts::sessions, QuotaResource::SESSIONS },
        Dimension { &QuotaAmounts::transferred_fds, QuotaResource::TRANSFERRED_FDS },
        Dimension { &QuotaAmounts::mapped_buffers, QuotaResource::MAPPED_BUFFERS },
        Dimension { &QuotaAmounts::mapped_bytes, QuotaResource::MAPPED_BYTES },
        Dimension { &QuotaAmounts::device_handles, QuotaResource::DEVICE_HANDLES },
        Dimension { &QuotaAmounts::completion_queues, QuotaResource::COMPLETION_QUEUES },
        Dimension { &QuotaAmounts::submission_queues, QuotaResource::SUBMISSION_QUEUES },
    };

    for (const auto& dimension : DIMENSIONS) {
        auto amounts = QuotaAmounts { };
        amounts.*dimension.amount = 5;

        QuotaManager principal_manager { enforced_policy(4, 9) };
        auto principal_result = principal_manager.acquire("camera", amounts);
        ASSERT_FALSE(principal_result.has_value());
        EXPECT_EQ(quota_exceeded(principal_result.error()).scope, QuotaScope::PRINCIPAL);
        EXPECT_EQ(quota_exceeded(principal_result.error()).resource, dimension.resource);
        EXPECT_EQ(quota_exceeded(principal_result.error()).principal, "camera");
        EXPECT_EQ(quota_exceeded(principal_result.error()).requested, 5);
        EXPECT_EQ(quota_exceeded(principal_result.error()).principal_current, 0);
        EXPECT_EQ(quota_exceeded(principal_result.error()).principal_limit, QuotaLimit { 4 });
        EXPECT_EQ(quota_exceeded(principal_result.error()).global_current, 0);
        EXPECT_EQ(quota_exceeded(principal_result.error()).global_limit, QuotaLimit { 9 });

        QuotaManager global_manager { enforced_policy(9, 4) };
        auto global_result = global_manager.acquire("camera", amounts);
        ASSERT_FALSE(global_result.has_value());
        EXPECT_EQ(quota_exceeded(global_result.error()).scope, QuotaScope::GLOBAL);
        EXPECT_EQ(quota_exceeded(global_result.error()).resource, dimension.resource);
        EXPECT_EQ(quota_exceeded(global_result.error()).principal_current, 0);
        EXPECT_EQ(quota_exceeded(global_result.error()).global_current, 0);
    }
}

TEST(QuotaManagerTest, OneAcquisitionChecksPrincipalAndGlobalLimitsAtomically)
{
    auto policy = enforced_policy(10, 10);
    policy.global.transferred_fds = 0;
    QuotaManager manager { std::move(policy) };

    auto denied = manager.acquire("camera", QuotaAmounts { .sessions = 3, .transferred_fds = 1 });
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(quota_exceeded(denied.error()).resource, QuotaResource::TRANSFERRED_FDS);
    EXPECT_TRUE(is_zero(manager.snapshot().global_usage.active));
    EXPECT_TRUE(manager.snapshot().principal_usage.empty());

    auto allowed = manager.acquire("camera", QuotaAmounts { .sessions = 10 });
    ASSERT_TRUE(allowed.has_value());
}

TEST(QuotaManagerTest, BufferCountSizeAndAggregateBytesDenyIndependently)
{
    auto count_policy = enforced_policy(100, 100);
    count_policy.default_principal.mapped_buffers = 0;
    QuotaManager count_manager { std::move(count_policy) };
    auto count_denied = count_manager.acquire_buffer("camera", 4);
    ASSERT_FALSE(count_denied.has_value());
    EXPECT_EQ(quota_exceeded(count_denied.error()).resource, QuotaResource::MAPPED_BUFFERS);

    auto size_policy = enforced_policy(100, 100);
    size_policy.default_principal.max_buffer_bytes = 3;
    QuotaManager size_manager { std::move(size_policy) };
    auto size_denied = size_manager.acquire_buffer("camera", 4);
    ASSERT_FALSE(size_denied.has_value());
    EXPECT_EQ(quota_exceeded(size_denied.error()).scope, QuotaScope::PRINCIPAL);
    EXPECT_EQ(quota_exceeded(size_denied.error()).resource, QuotaResource::MAX_BUFFER_BYTES);
    EXPECT_EQ(quota_exceeded(size_denied.error()).requested, 4);
    EXPECT_EQ(quota_exceeded(size_denied.error()).principal_current, std::nullopt);
    EXPECT_EQ(quota_exceeded(size_denied.error()).global_current, std::nullopt);
    EXPECT_EQ(quota_exceeded(size_denied.error()).principal_limit, QuotaLimit { 3 });
    EXPECT_EQ(quota_exceeded(size_denied.error()).global_limit, QuotaLimit { 100 });

    auto aggregate_policy = enforced_policy(100, 100);
    aggregate_policy.default_principal.mapped_bytes = 3;
    QuotaManager aggregate_manager { std::move(aggregate_policy) };
    auto aggregate_denied = aggregate_manager.acquire_buffer("camera", 4);
    ASSERT_FALSE(aggregate_denied.has_value());
    EXPECT_EQ(quota_exceeded(aggregate_denied.error()).resource, QuotaResource::MAPPED_BYTES);
}

TEST(QuotaManagerTest, UsageAggregatesAcrossLeasesForOnePrincipal)
{
    QuotaManager manager { enforced_policy(20, 20) };
    auto first = manager.acquire("camera", QuotaAmounts { .sessions = 2, .device_handles = 3 });
    auto second = manager.acquire("camera", QuotaAmounts { .sessions = 5, .device_handles = 7 });
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());

    const auto snapshot = manager.snapshot();
    EXPECT_EQ(snapshot.principal_usage.at("camera").active.sessions, 7);
    EXPECT_EQ(snapshot.principal_usage.at("camera").active.device_handles, 10);
    EXPECT_EQ(snapshot.global_usage.active.sessions, 7);
    EXPECT_EQ(snapshot.global_usage.active.device_handles, 10);
}

TEST(QuotaManagerTest, PrincipalsAndDimensionsRemainIndependent)
{
    QuotaManager manager { enforced_policy(5, 20) };
    auto camera_sessions = manager.acquire("camera", QuotaAmounts { .sessions = 5 });
    auto storage_fds = manager.acquire("storage", QuotaAmounts { .transferred_fds = 5 });
    ASSERT_TRUE(camera_sessions.has_value());
    ASSERT_TRUE(storage_fds.has_value());

    auto camera_denied = manager.acquire("camera", QuotaAmounts { .sessions = 1 });
    EXPECT_FALSE(camera_denied.has_value());
    auto storage_allowed = manager.acquire("storage", QuotaAmounts { .sessions = 1 });
    EXPECT_TRUE(storage_allowed.has_value());
    auto camera_other_dimension = manager.acquire("camera", QuotaAmounts { .transferred_fds = 1 });
    EXPECT_TRUE(camera_other_dimension.has_value());
}

TEST(QuotaManagerTest, UnlimitedModeAccountsWithoutPolicyDenial)
{
    auto policy = enforced_policy(0, 0);
    policy.mode = QuotaMode::UNLIMITED;
    QuotaManager manager { std::move(policy) };

    auto lease = manager.acquire("camera",
        QuotaAmounts {
            .sessions = std::numeric_limits<uint64_t>::max(),
            .transferred_fds = 1,
        });
    ASSERT_TRUE(lease.has_value());
    EXPECT_EQ(manager.snapshot().global_usage.active, lease->amounts());
}

TEST(QuotaManagerTest, UnlimitedModeReportsAccountingOverflowWithoutChangingUsage)
{
    constexpr auto MAX = std::numeric_limits<uint64_t>::max();
    QuotaManager manager { QuotaPolicy { } };
    auto existing = manager.acquire("camera", QuotaAmounts { .sessions = MAX });
    ASSERT_TRUE(existing.has_value());
    const auto before = manager.snapshot();

    auto denied = manager.acquire("camera", QuotaAmounts { .sessions = 1 });

    ASSERT_FALSE(denied.has_value());
    const auto* overflow = std::get_if<QuotaAccountingOverflow>(&denied.error());
    ASSERT_NE(overflow, nullptr);
    EXPECT_EQ(overflow->scope, QuotaScope::PRINCIPAL);
    EXPECT_EQ(overflow->resource, QuotaResource::SESSIONS);
    EXPECT_EQ(overflow->principal, "camera");
    EXPECT_EQ(overflow->requested, 1);
    EXPECT_EQ(overflow->current, MAX);
    EXPECT_EQ(manager.snapshot().global_usage, before.global_usage);
    EXPECT_EQ(manager.snapshot().principal_usage, before.principal_usage);
}

TEST(QuotaManagerTest, PerFieldUnlimitedReportsGlobalAccountingOverflow)
{
    constexpr auto MAX = std::numeric_limits<uint64_t>::max();
    auto policy = enforced_policy(MAX, MAX);
    policy.default_principal.mapped_bytes = std::nullopt;
    policy.global.mapped_bytes = std::nullopt;
    QuotaManager manager { std::move(policy) };
    auto existing = manager.acquire("camera", QuotaAmounts { .mapped_bytes = MAX });
    ASSERT_TRUE(existing.has_value());
    const auto before = manager.snapshot();

    auto denied = manager.acquire("storage", QuotaAmounts { .mapped_bytes = 1 });

    ASSERT_FALSE(denied.has_value());
    const auto* overflow = std::get_if<QuotaAccountingOverflow>(&denied.error());
    ASSERT_NE(overflow, nullptr);
    EXPECT_EQ(overflow->scope, QuotaScope::GLOBAL);
    EXPECT_EQ(overflow->resource, QuotaResource::MAPPED_BYTES);
    EXPECT_EQ(overflow->principal, "storage");
    EXPECT_EQ(overflow->requested, 1);
    EXPECT_EQ(overflow->current, MAX);
    EXPECT_EQ(manager.snapshot().global_usage, before.global_usage);
    EXPECT_EQ(manager.snapshot().principal_usage, before.principal_usage);
}

TEST(QuotaManagerTest, PerFieldUnlimitedCoexistsWithFiniteLimits)
{
    auto policy = enforced_policy(1, 20);
    policy.default_principal.sessions = std::nullopt;
    QuotaManager manager { std::move(policy) };

    auto unlimited = manager.acquire("camera", QuotaAmounts { .sessions = 10 });
    ASSERT_TRUE(unlimited.has_value());
    auto finite = manager.acquire("camera", QuotaAmounts { .transferred_fds = 2 });
    ASSERT_FALSE(finite.has_value());
    EXPECT_EQ(quota_exceeded(finite.error()).resource, QuotaResource::TRANSFERRED_FDS);
}

TEST(QuotaManagerTest, ZeroDeniesPositiveAcquisition)
{
    QuotaManager manager { enforced_policy(0, 0) };

    auto denied = manager.acquire("camera", QuotaAmounts { .completion_queues = 1 });
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(quota_exceeded(denied.error()).scope, QuotaScope::PRINCIPAL);
    EXPECT_EQ(quota_exceeded(denied.error()).resource, QuotaResource::COMPLETION_QUEUES);
}

TEST(QuotaManagerTest, LeaseDestructionReleasesExactAmounts)
{
    QuotaManager manager { enforced_policy(20, 20) };
    {
        auto lease = manager.acquire("camera", QuotaAmounts { .sessions = 2, .submission_queues = 3 });
        ASSERT_TRUE(lease.has_value());
        EXPECT_EQ(manager.snapshot().global_usage.active.sessions, 2);
    }

    EXPECT_TRUE(is_zero(manager.snapshot().global_usage.active));
    EXPECT_TRUE(manager.snapshot().principal_usage.empty());
}

TEST(QuotaManagerTest, MoveConstructionAndAssignmentReleaseExactlyOnce)
{
    QuotaManager manager { enforced_policy(20, 20) };
    auto source_result = manager.acquire("camera", QuotaAmounts { .sessions = 2 });
    auto destination_result = manager.acquire("camera", QuotaAmounts { .sessions = 3 });
    ASSERT_TRUE(source_result.has_value());
    ASSERT_TRUE(destination_result.has_value());

    {
        QuotaLease source { std::move(*source_result) };
        QuotaLease destination { std::move(*destination_result) };
        destination = std::move(source);
        EXPECT_EQ(manager.snapshot().global_usage.active.sessions, 2);
    }
    EXPECT_EQ(manager.snapshot().global_usage.active.sessions, 0);
    EXPECT_TRUE(manager.snapshot().principal_usage.empty());
}

TEST(QuotaManagerTest, FailedAcquisitionDoesNotChangeUsage)
{
    constexpr auto MAX = std::numeric_limits<uint64_t>::max();
    QuotaManager manager { enforced_policy(MAX, MAX) };
    auto existing = manager.acquire("camera", QuotaAmounts { .mapped_bytes = MAX });
    ASSERT_TRUE(existing.has_value());

    const auto before = manager.snapshot();
    auto denied = manager.acquire("camera", QuotaAmounts { .mapped_bytes = 1 });
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(quota_exceeded(denied.error()).resource, QuotaResource::MAPPED_BYTES);
    EXPECT_EQ(quota_exceeded(denied.error()).scope, QuotaScope::PRINCIPAL);
    const auto after = manager.snapshot();
    EXPECT_EQ(after.global_usage, before.global_usage);
    EXPECT_EQ(after.principal_usage, before.principal_usage);
}

TEST(QuotaManagerTest, OrphanMovesActiveUsageAndPersistsAfterLeaseDestruction)
{
    auto policy = enforced_policy(20, 20);
    policy.default_principal.mapped_buffers = 2;
    policy.default_principal.mapped_bytes = 9;
    policy.global.mapped_buffers = 2;
    policy.global.mapped_bytes = 9;
    QuotaManager manager { std::move(policy) };
    {
        auto lease = manager.acquire("camera", QuotaAmounts { .mapped_buffers = 2, .mapped_bytes = 9 });
        ASSERT_TRUE(lease.has_value());
        lease->mark_orphaned();
        lease->mark_orphaned();
        const auto snapshot = manager.snapshot();
        EXPECT_TRUE(is_zero(snapshot.global_usage.active));
        EXPECT_EQ(snapshot.global_usage.orphan.mapped_buffers, 2);
        EXPECT_EQ(snapshot.global_usage.orphan.mapped_bytes, 9);
    }

    const auto snapshot = manager.snapshot();
    EXPECT_EQ(snapshot.principal_usage.at("camera").orphan.mapped_buffers, 2);
    EXPECT_EQ(snapshot.principal_usage.at("camera").orphan.mapped_bytes, 9);
    auto denied = manager.acquire_buffer("camera", 1);
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(quota_exceeded(denied.error()).resource, QuotaResource::MAPPED_BUFFERS);
    EXPECT_EQ(quota_exceeded(denied.error()).principal_current, 2);
}

TEST(QuotaManagerTest, SnapshotContainsGenerationModesLimitsAndUsage)
{
    auto policy = enforced_policy(10, 20);
    policy.principals.emplace("camera", finite_limits(3));
    const auto expected_policy = policy;
    QuotaManager manager { std::move(policy) };
    auto active = manager.acquire("camera", QuotaAmounts { .sessions = 2 });
    auto orphan = manager.acquire("storage", QuotaAmounts { .device_handles = 1 });
    ASSERT_TRUE(active.has_value());
    ASSERT_TRUE(orphan.has_value());
    orphan->mark_orphaned();

    const auto snapshot = manager.snapshot();
    EXPECT_EQ(snapshot.generation, 1);
    EXPECT_EQ(snapshot.mode, QuotaMode::ENFORCED);
    EXPECT_EQ(snapshot.global_limits, expected_policy.global);
    EXPECT_EQ(snapshot.default_limits, expected_policy.default_principal);
    EXPECT_EQ(snapshot.named_limits, expected_policy.principals);
    EXPECT_EQ(snapshot.global_usage.active.sessions, 2);
    EXPECT_EQ(snapshot.global_usage.orphan.device_handles, 1);
    EXPECT_EQ(snapshot.principal_usage.at("camera").active.sessions, 2);
    EXPECT_EQ(snapshot.principal_usage.at("storage").orphan.device_handles, 1);
}

TEST(QuotaManagerTest, LoweredLimitsGrandfatherUsageAndBlockOnlyGrowth)
{
    QuotaManager manager { enforced_policy(10, 10) };
    auto existing = manager.acquire("camera", QuotaAmounts { .sessions = 8 });
    ASSERT_TRUE(existing.has_value());

    ASSERT_TRUE(manager.replace_policy(enforced_policy(3, 3)).has_value());
    auto no_growth = manager.acquire("camera", QuotaAmounts { });
    EXPECT_TRUE(no_growth.has_value());
    auto growth = manager.acquire("camera", QuotaAmounts { .sessions = 1 });
    ASSERT_FALSE(growth.has_value());
    EXPECT_EQ(quota_exceeded(growth.error()).principal_current, 8);
    EXPECT_EQ(quota_exceeded(growth.error()).principal_limit, QuotaLimit { 3 });

    existing = std::unexpected(QuotaAcquireError { QuotaExceeded { } });
    auto within_new_limit = manager.acquire("camera", QuotaAmounts { .sessions = 3 });
    EXPECT_TRUE(within_new_limit.has_value());
}

TEST(QuotaManagerTest, ReloadSupportsFiniteAndUnlimitedTransitions)
{
    QuotaManager manager { enforced_policy(0, 0) };
    EXPECT_FALSE(manager.acquire("camera", QuotaAmounts { .sessions = 1 }).has_value());

    auto unlimited = enforced_policy(0, 0);
    unlimited.mode = QuotaMode::UNLIMITED;
    ASSERT_TRUE(manager.replace_policy(unlimited).has_value());
    auto accounted = manager.acquire("camera", QuotaAmounts { .sessions = 1 });
    ASSERT_TRUE(accounted.has_value());
    EXPECT_EQ(manager.snapshot().generation, 2);

    auto finite = enforced_policy(1, 1);
    ASSERT_TRUE(manager.replace_policy(finite).has_value());
    EXPECT_EQ(manager.snapshot().generation, 3);
    EXPECT_FALSE(manager.acquire("camera", QuotaAmounts { .sessions = 1 }).has_value());
}

TEST(QuotaManagerTest, ReloadRejectsRemovalOfPrincipalWithActiveOrOrphanUsage)
{
    auto policy = enforced_policy(20, 20);
    policy.principals.emplace("active", finite_limits(10));
    policy.principals.emplace("orphan", finite_limits(10));
    policy.principals.emplace("idle", finite_limits(10));
    QuotaManager manager { policy };
    auto active = manager.acquire("active", QuotaAmounts { .sessions = 1 });
    auto orphan = manager.acquire("orphan", QuotaAmounts { .mapped_bytes = 1 });
    ASSERT_TRUE(active.has_value());
    ASSERT_TRUE(orphan.has_value());
    orphan->mark_orphaned();

    auto remove_active = policy;
    remove_active.principals.erase("active");
    auto active_error = manager.replace_policy(std::move(remove_active));
    ASSERT_FALSE(active_error.has_value());
    EXPECT_EQ(active_error.error().principal, "active");
    EXPECT_FALSE(active_error.error().message.empty());
    EXPECT_EQ(manager.snapshot().generation, 1);

    auto remove_orphan = policy;
    remove_orphan.principals.erase("orphan");
    auto orphan_error = manager.replace_policy(std::move(remove_orphan));
    ASSERT_FALSE(orphan_error.has_value());
    EXPECT_EQ(orphan_error.error().principal, "orphan");
    EXPECT_EQ(manager.snapshot().generation, 1);

    auto remove_idle = policy;
    remove_idle.principals.erase("idle");
    EXPECT_TRUE(manager.replace_policy(std::move(remove_idle)).has_value());
    EXPECT_EQ(manager.snapshot().generation, 2);
}

TEST(QuotaManagerTest, DeterministicLogicalSessionInterleavingStaysConsistent)
{
    QuotaManager manager { enforced_policy(20, 40) };
    auto camera_session = manager.acquire("camera", QuotaAmounts { .sessions = 1 });
    auto storage_session = manager.acquire("storage", QuotaAmounts { .sessions = 1 });
    ASSERT_TRUE(camera_session.has_value());
    ASSERT_TRUE(storage_session.has_value());
    auto camera_buffer = manager.acquire_buffer("camera", 7);
    auto storage_queue = manager.acquire("storage",
        QuotaAmounts {
            .completion_queues = 1,
            .submission_queues = 1,
        });
    ASSERT_TRUE(camera_buffer.has_value());
    ASSERT_TRUE(storage_queue.has_value());

    camera_buffer->mark_orphaned();
    storage_queue = std::unexpected(QuotaAcquireError { QuotaExceeded { } });
    auto snapshot = manager.snapshot();
    EXPECT_EQ(snapshot.global_usage.active.sessions, 2);
    EXPECT_EQ(snapshot.global_usage.active.completion_queues, 0);
    EXPECT_EQ(snapshot.global_usage.active.submission_queues, 0);
    EXPECT_EQ(snapshot.global_usage.orphan.mapped_buffers, 1);
    EXPECT_EQ(snapshot.global_usage.orphan.mapped_bytes, 7);
    EXPECT_EQ(snapshot.principal_usage.at("camera").active.sessions, 1);
    EXPECT_EQ(snapshot.principal_usage.at("storage").active.sessions, 1);

    camera_session = std::unexpected(QuotaAcquireError { QuotaExceeded { } });
    storage_session = std::unexpected(QuotaAcquireError { QuotaExceeded { } });
    snapshot = manager.snapshot();
    EXPECT_TRUE(is_zero(snapshot.global_usage.active));
    EXPECT_EQ(snapshot.global_usage.orphan.mapped_bytes, 7);
}
