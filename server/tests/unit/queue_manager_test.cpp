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

#include "mock_nvme_backend.hpp"
#include "queue_manager.hpp"
#include "quota_manager.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::_;
using ::testing::Return;

class QueueManagerTest : public ::testing::Test {
protected:
    QuotaLease cq_lease() { return std::move(*quotas_.acquire("test", QuotaAmounts { .completion_queues = 1 })); }

    QuotaLease sq_lease() { return std::move(*quotas_.acquire("test", QuotaAmounts { .submission_queues = 1 })); }

    ::testing::NiceMock<MockNvmeBackend> mock_;
    QuotaManager quotas_ { QuotaPolicy { } };
    QueueManager qm_ { mock_ };
    void* const CTRLR = reinterpret_cast<void*>(0xABCD);
    void* const OTHER_CTRLR = reinterpret_cast<void*>(0xDCBA);
    SleepFn no_sleep_ = [](auto) { };
};

TEST_F(QueueManagerTest, CreateCqReturnsCqIdAndDbOffset)
{
    constexpr uint64_t DB_OFFSET = 0x5000;

    EXPECT_CALL(mock_, create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, DB_OFFSET }));

    auto result = qm_.create_cq(CTRLR, 1, 0xBEEF000, 64, no_sleep_, cq_lease());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->qid, 1u);
    EXPECT_EQ(result->db_offset, DB_OFFSET);
    EXPECT_TRUE(qm_.has_cq(1));
}

TEST_F(QueueManagerTest, CreateCqPropagatesLenderError)
{
    EXPECT_CALL(mock_, create_cq(_, _, _, _)).WillOnce(Return(std::unexpected(-1)));

    auto result = qm_.create_cq(CTRLR, 1, 0, 64, no_sleep_, cq_lease());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::INTERNAL);
    EXPECT_FALSE(qm_.has_cq(1));
}

TEST_F(QueueManagerTest, DeleteCqNotFoundOnBadId)
{
    auto result = qm_.delete_cq(CTRLR, 99, no_sleep_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::NOT_FOUND);
}

TEST_F(QueueManagerTest, DeleteCqSucceeds)
{
    EXPECT_CALL(mock_, create_cq(_, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, delete_cq(_, 1, _)).WillOnce(Return(std::expected<void, int> { }));

    qm_.create_cq(CTRLR, 1, 0, 64, no_sleep_, cq_lease());

    auto result = qm_.delete_cq(CTRLR, 1, no_sleep_);

    EXPECT_TRUE(result.has_value());
    EXPECT_FALSE(qm_.has_cq(1));
}

TEST_F(QueueManagerTest, HasSqsForCqReturnsTrueWhenSqExists)
{
    EXPECT_CALL(mock_, create_cq(_, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, create_sq(_, _, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));

    qm_.create_cq(CTRLR, 1, 0, 64, no_sleep_, cq_lease());
    qm_.create_sq(CTRLR, 1, 1, 0, 64, no_sleep_, sq_lease());

    EXPECT_TRUE(qm_.has_sqs_for_cq(1));
    EXPECT_FALSE(qm_.has_sqs_for_cq(99));
}

TEST_F(QueueManagerTest, HasQueuesOnDeviceTracksCompletionAndSubmissionQueues)
{
    EXPECT_CALL(mock_, create_cq(_, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, create_sq(_, _, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 2, 0 }));
    EXPECT_CALL(mock_, delete_sq(_, 2, _)).WillOnce(Return(std::expected<void, int> { }));
    EXPECT_CALL(mock_, delete_cq(_, 1, _)).WillOnce(Return(std::expected<void, int> { }));

    EXPECT_FALSE(qm_.has_queues_on_device(1));
    qm_.create_cq(CTRLR, 1, 0, 64, no_sleep_, cq_lease());
    qm_.create_sq(CTRLR, 2, 1, 0, 64, no_sleep_, sq_lease());

    EXPECT_TRUE(qm_.has_queues_on_device(1));
    EXPECT_TRUE(qm_.has_queues_on_device(2));
    EXPECT_FALSE(qm_.has_queues_on_device(3));

    qm_.delete_sq(CTRLR, 2, no_sleep_);
    EXPECT_FALSE(qm_.has_queues_on_device(2));
    qm_.delete_cq(CTRLR, 1, no_sleep_);
    EXPECT_FALSE(qm_.has_queues_on_device(1));
}

TEST_F(QueueManagerTest, CreateSqReturnsSqIdAndDbOffset)
{
    constexpr uint64_t DB_OFFSET = 0x6000;

    EXPECT_CALL(mock_, create_cq(_, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, create_sq(CTRLR, _, _, 1, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, DB_OFFSET }));

    qm_.create_cq(CTRLR, 1, 0, 64, no_sleep_, cq_lease());

    auto result = qm_.create_sq(CTRLR, 1, 1, 0, 64, no_sleep_, sq_lease());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->qid, 1u);
    EXPECT_EQ(result->db_offset, DB_OFFSET);
}

TEST_F(QueueManagerTest, DeviceOfCqReturnsZeroOnMissing) { EXPECT_EQ(qm_.device_of_cq(42), 0u); }

TEST_F(QueueManagerTest, DeviceOfSqReturnsZeroOnMissing) { EXPECT_EQ(qm_.device_of_sq(42), 0u); }

TEST_F(QueueManagerTest, DeleteSqNotFoundOnBadId)
{
    auto result = qm_.delete_sq(CTRLR, 99, no_sleep_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::NOT_FOUND);
}

TEST_F(QueueManagerTest, DeleteSqSucceeds)
{
    EXPECT_CALL(mock_, create_cq(_, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, create_sq(_, _, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, delete_sq(_, 1, _)).WillOnce(Return(std::expected<void, int> { }));

    qm_.create_cq(CTRLR, 1, 0, 64, no_sleep_, cq_lease());
    qm_.create_sq(CTRLR, 1, 1, 0, 64, no_sleep_, sq_lease());

    auto result = qm_.delete_sq(CTRLR, 1, no_sleep_);

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(qm_.device_of_sq(1), 0u);
}

TEST_F(QueueManagerTest, DeleteCqFailsWhenBackendFails)
{
    EXPECT_CALL(mock_, create_cq(_, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, delete_cq(_, 1, _)).WillOnce(Return(std::unexpected(-1)));

    qm_.create_cq(CTRLR, 1, 0, 64, no_sleep_, cq_lease());
    auto result = qm_.delete_cq(CTRLR, 1, no_sleep_);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::INTERNAL);
    EXPECT_TRUE(qm_.has_cq(1));
}

TEST_F(QueueManagerTest, CreateSqFailsWhenBackendFails)
{
    EXPECT_CALL(mock_, create_cq(_, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, create_sq(_, _, _, _, _)).WillOnce(Return(std::unexpected(-1)));

    qm_.create_cq(CTRLR, 1, 0, 64, no_sleep_, cq_lease());
    auto result = qm_.create_sq(CTRLR, 1, 1, 0, 64, no_sleep_, sq_lease());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::INTERNAL);
}

TEST_F(QueueManagerTest, DeleteSqFailsWhenBackendFails)
{
    EXPECT_CALL(mock_, create_cq(_, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, create_sq(_, _, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, delete_sq(_, 1, _)).WillOnce(Return(std::unexpected(-1)));

    qm_.create_cq(CTRLR, 1, 0, 64, no_sleep_, cq_lease());
    qm_.create_sq(CTRLR, 1, 1, 0, 64, no_sleep_, sq_lease());
    auto result = qm_.delete_sq(CTRLR, 1, no_sleep_);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::INTERNAL);
    EXPECT_NE(qm_.device_of_sq(1), 0u);
}

TEST_F(QueueManagerTest, SuccessfulCreateRetainsCqLeaseUntilDelete)
{
    EXPECT_CALL(mock_, create_cq(_, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, delete_cq(_, 1, _)).WillOnce(Return(std::expected<void, int> { }));

    ASSERT_TRUE(qm_.create_cq(CTRLR, 1, 0, 64, no_sleep_, cq_lease()).has_value());
    EXPECT_EQ(quotas_.snapshot().global_usage.active.completion_queues, 1u);

    ASSERT_TRUE(qm_.delete_cq(CTRLR, 1, no_sleep_).has_value());
    EXPECT_EQ(quotas_.snapshot().global_usage.active.completion_queues, 0u);
}

TEST_F(QueueManagerTest, FailedCreateCqReleasesLease)
{
    EXPECT_CALL(mock_, create_cq(_, _, _, _)).WillOnce(Return(std::unexpected(-1)));

    EXPECT_FALSE(qm_.create_cq(CTRLR, 1, 0, 64, no_sleep_, cq_lease()).has_value());

    const auto usage = quotas_.snapshot().global_usage;
    EXPECT_EQ(usage.active.completion_queues, 0u);
    EXPECT_EQ(usage.orphan.completion_queues, 0u);
}

TEST_F(QueueManagerTest, FailedDeleteCqRetainsLease)
{
    EXPECT_CALL(mock_, create_cq(_, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, delete_cq(_, 1, _)).WillOnce(Return(std::unexpected(-1)));

    ASSERT_TRUE(qm_.create_cq(CTRLR, 1, 0, 64, no_sleep_, cq_lease()).has_value());
    EXPECT_FALSE(qm_.delete_cq(CTRLR, 1, no_sleep_).has_value());

    const auto usage = quotas_.snapshot().global_usage;
    EXPECT_EQ(usage.active.completion_queues, 1u);
    EXPECT_EQ(usage.orphan.completion_queues, 0u);
}

TEST_F(QueueManagerTest, SuccessfulCreateRetainsSqLeaseUntilDelete)
{
    EXPECT_CALL(mock_, create_sq(_, _, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, delete_sq(_, 1, _)).WillOnce(Return(std::expected<void, int> { }));

    ASSERT_TRUE(qm_.create_sq(CTRLR, 1, 7, 0, 64, no_sleep_, sq_lease()).has_value());
    EXPECT_EQ(quotas_.snapshot().global_usage.active.submission_queues, 1u);

    ASSERT_TRUE(qm_.delete_sq(CTRLR, 1, no_sleep_).has_value());
    EXPECT_EQ(quotas_.snapshot().global_usage.active.submission_queues, 0u);
}

TEST_F(QueueManagerTest, FailedCreateSqReleasesLease)
{
    EXPECT_CALL(mock_, create_sq(_, _, _, _, _)).WillOnce(Return(std::unexpected(-1)));

    EXPECT_FALSE(qm_.create_sq(CTRLR, 1, 7, 0, 64, no_sleep_, sq_lease()).has_value());

    const auto usage = quotas_.snapshot().global_usage;
    EXPECT_EQ(usage.active.submission_queues, 0u);
    EXPECT_EQ(usage.orphan.submission_queues, 0u);
}

TEST_F(QueueManagerTest, FailedDeleteSqRetainsLease)
{
    EXPECT_CALL(mock_, create_sq(_, _, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, delete_sq(_, 1, _)).WillOnce(Return(std::unexpected(-1)));

    ASSERT_TRUE(qm_.create_sq(CTRLR, 1, 7, 0, 64, no_sleep_, sq_lease()).has_value());
    EXPECT_FALSE(qm_.delete_sq(CTRLR, 1, no_sleep_).has_value());

    const auto usage = quotas_.snapshot().global_usage;
    EXPECT_EQ(usage.active.submission_queues, 1u);
    EXPECT_EQ(usage.orphan.submission_queues, 0u);
}

TEST_F(QueueManagerTest, OrphanCqAndSqMoveChargesToOrphanUsage)
{
    EXPECT_CALL(mock_, create_cq(_, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, create_sq(_, _, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 2, 0 }));

    ASSERT_TRUE(qm_.create_cq(CTRLR, 1, 0, 64, no_sleep_, cq_lease()).has_value());
    ASSERT_TRUE(qm_.create_sq(CTRLR, 1, 1, 0, 64, no_sleep_, sq_lease()).has_value());

    qm_.orphan_sq(2);
    qm_.orphan_cq(1);

    const auto usage = quotas_.snapshot().global_usage;
    EXPECT_EQ(usage.active.completion_queues, 0u);
    EXPECT_EQ(usage.active.submission_queues, 0u);
    EXPECT_EQ(usage.orphan.completion_queues, 1u);
    EXPECT_EQ(usage.orphan.submission_queues, 1u);
}

TEST_F(QueueManagerTest, DestructorOrphansEntriesThatCleanupDidNotRemove)
{
    EXPECT_CALL(mock_, create_cq(_, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, create_sq(_, _, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 2, 0 }));

    {
        QueueManager manager { mock_ };
        ASSERT_TRUE(manager.create_cq(CTRLR, 1, 0, 64, no_sleep_, cq_lease()).has_value());
        ASSERT_TRUE(manager.create_sq(CTRLR, 1, 1, 0, 64, no_sleep_, sq_lease()).has_value());
    }

    const auto usage = quotas_.snapshot().global_usage;
    EXPECT_EQ(usage.active.completion_queues, 0u);
    EXPECT_EQ(usage.active.submission_queues, 0u);
    EXPECT_EQ(usage.orphan.completion_queues, 1u);
    EXPECT_EQ(usage.orphan.submission_queues, 1u);
}

TEST_F(QueueManagerTest, DuplicateCqIdReleasesAttemptedLeaseAfterSuccessfulRollback)
{
    EXPECT_CALL(mock_, create_cq(_, _, _, _))
        .WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }))
        .WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, delete_cq(OTHER_CTRLR, 1, _)).WillOnce(Return(std::expected<void, int> { }));

    ASSERT_TRUE(qm_.create_cq(CTRLR, 1, 0, 64, no_sleep_, cq_lease()).has_value());
    EXPECT_FALSE(qm_.create_cq(OTHER_CTRLR, 2, 0, 64, no_sleep_, cq_lease()).has_value());

    const auto usage = quotas_.snapshot().global_usage;
    EXPECT_EQ(usage.active.completion_queues, 1u);
    EXPECT_EQ(usage.orphan.completion_queues, 0u);
}

TEST_F(QueueManagerTest, DuplicateCqIdOrphansAttemptedLeaseAfterFailedRollback)
{
    EXPECT_CALL(mock_, create_cq(_, _, _, _))
        .WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }))
        .WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, delete_cq(OTHER_CTRLR, 1, _)).WillOnce(Return(std::unexpected(-1)));

    ASSERT_TRUE(qm_.create_cq(CTRLR, 1, 0, 64, no_sleep_, cq_lease()).has_value());
    EXPECT_FALSE(qm_.create_cq(OTHER_CTRLR, 2, 0, 64, no_sleep_, cq_lease()).has_value());

    const auto usage = quotas_.snapshot().global_usage;
    EXPECT_EQ(usage.active.completion_queues, 1u);
    EXPECT_EQ(usage.orphan.completion_queues, 1u);
}

TEST_F(QueueManagerTest, DuplicateSqIdReleasesAttemptedLeaseAfterSuccessfulRollback)
{
    EXPECT_CALL(mock_, create_sq(_, _, _, _, _))
        .WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }))
        .WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, delete_sq(OTHER_CTRLR, 1, _)).WillOnce(Return(std::expected<void, int> { }));

    ASSERT_TRUE(qm_.create_sq(CTRLR, 1, 7, 0, 64, no_sleep_, sq_lease()).has_value());
    EXPECT_FALSE(qm_.create_sq(OTHER_CTRLR, 2, 7, 0, 64, no_sleep_, sq_lease()).has_value());

    const auto usage = quotas_.snapshot().global_usage;
    EXPECT_EQ(usage.active.submission_queues, 1u);
    EXPECT_EQ(usage.orphan.submission_queues, 0u);
}

TEST_F(QueueManagerTest, DuplicateSqIdOrphansAttemptedLeaseAfterFailedRollback)
{
    EXPECT_CALL(mock_, create_sq(_, _, _, _, _))
        .WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }))
        .WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(mock_, delete_sq(OTHER_CTRLR, 1, _)).WillOnce(Return(std::unexpected(-1)));

    ASSERT_TRUE(qm_.create_sq(CTRLR, 1, 7, 0, 64, no_sleep_, sq_lease()).has_value());
    EXPECT_FALSE(qm_.create_sq(OTHER_CTRLR, 2, 7, 0, 64, no_sleep_, sq_lease()).has_value());

    const auto usage = quotas_.snapshot().global_usage;
    EXPECT_EQ(usage.active.submission_queues, 1u);
    EXPECT_EQ(usage.orphan.submission_queues, 1u);
}
