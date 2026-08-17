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

#include "command_metrics.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace {

using namespace std::chrono_literals;
using MethodId = nvidia::storage_lender::v1::MethodId;
using StatusCode = nvidia::storage_lender::wire::v1::StatusCode;

class ManualTimer final : public CommandMetricsTimer {
public:
    void async_wait_until(TimePoint deadline, Handler handler) override
    {
        deadline_ = deadline;
        handler_ = std::move(handler);
    }

    void cancel() override
    {
        ++cancel_count_;
        handler_ = { };
        deadline_.reset();
    }

    void fire(boost::system::error_code error = { })
    {
        auto handler = std::exchange(handler_, { });
        deadline_.reset();
        ASSERT_TRUE(handler);
        handler(error);
    }

    std::optional<TimePoint> deadline_;
    Handler handler_;
    uint32_t cancel_count_ { };
};

struct ManualClocks {
    std::chrono::steady_clock::time_point steady { std::chrono::steady_clock::now() };
    std::chrono::system_clock::time_point system { std::chrono::system_clock::time_point {
        std::chrono::seconds { 1'700'000'000 } } };
};

class CommandMetricsTest : public ::testing::Test {
protected:
    CommandMetricsTest()
        : metrics_ { make_timer(), [this] { return clocks_.steady; }, [this] { return clocks_.system; } }
    {
    }

    std::unique_ptr<CommandMetricsTimer> make_timer()
    {
        auto timer = std::make_unique<ManualTimer>();
        timer_ = timer.get();
        return timer;
    }

    void advance(std::chrono::microseconds duration)
    {
        clocks_.steady += duration;
        clocks_.system += duration;
    }

    void record(MethodId method, std::chrono::microseconds duration, StatusCode status = StatusCode::OK)
    {
        const auto measurement = metrics_.begin(static_cast<uint32_t>(method));
        advance(duration);
        metrics_.complete(measurement, status);
    }

    const CommandLatencySummary& summary(std::string_view command)
    {
        snapshot_ = metrics_.snapshot();
        const auto found = std::ranges::find(snapshot_.commands, command, &CommandLatencySummary::command);
        EXPECT_NE(found, snapshot_.commands.end());
        return *found;
    }

    ManualClocks clocks_;
    ManualTimer* timer_ { };
    CommandMetrics metrics_;
    CommandMetricsSnapshot snapshot_;
};

TEST_F(CommandMetricsTest, EmptySnapshotContainsKnownCommandsWithoutValues)
{
    const auto snapshot = metrics_.snapshot();

    EXPECT_EQ(snapshot.capacity_per_command, 256U);
    ASSERT_EQ(snapshot.commands.size(), 10U);
    EXPECT_EQ(snapshot.commands.front().command, "transfer_fd");
    EXPECT_EQ(snapshot.commands.back().command, "delete_submission_queue");
    for (const auto& command : snapshot.commands) {
        EXPECT_EQ(command.samples, 0U);
        EXPECT_EQ(command.errors, 0U);
        EXPECT_EQ(command.peak_in_flight, 0U);
        EXPECT_FALSE(command.p50_us);
        EXPECT_FALSE(command.p90_us);
        EXPECT_FALSE(command.p99_us);
        EXPECT_FALSE(command.max_us);
        EXPECT_FALSE(command.event_loop_lag_p90_us);
        EXPECT_FALSE(command.event_loop_lag_max_us);
        EXPECT_FALSE(command.oldest_completed_unix_ms);
        EXPECT_FALSE(command.newest_completed_unix_ms);
    }
}

TEST_F(CommandMetricsTest, RecordsDurationStatusConcurrencyAndTimestamps)
{
    const auto first = metrics_.begin(static_cast<uint32_t>(MethodId::MAP_BUFFER));
    advance(1ms);
    const auto second = metrics_.begin(static_cast<uint32_t>(MethodId::MAP_BUFFER));
    advance(2ms);
    metrics_.complete(second, StatusCode::INVALID_ARGUMENT);
    advance(3ms);
    metrics_.complete(first, StatusCode::OK);

    const auto& command = summary("map_buffer");
    EXPECT_EQ(command.samples, 2U);
    EXPECT_EQ(command.errors, 1U);
    EXPECT_EQ(command.peak_in_flight, 2U);
    ASSERT_TRUE(command.p50_us);
    ASSERT_TRUE(command.p90_us);
    ASSERT_TRUE(command.p99_us);
    ASSERT_TRUE(command.max_us);
    EXPECT_EQ(*command.p50_us, 2'000U);
    EXPECT_EQ(*command.p90_us, 6'000U);
    EXPECT_EQ(*command.p99_us, 6'000U);
    EXPECT_EQ(*command.max_us, 6'000U);
    ASSERT_TRUE(command.oldest_completed_unix_ms);
    ASSERT_TRUE(command.newest_completed_unix_ms);
    EXPECT_EQ(*command.oldest_completed_unix_ms, 1'700'000'000'003U);
    EXPECT_EQ(*command.newest_completed_unix_ms, 1'700'000'000'006U);
}

TEST_F(CommandMetricsTest, UsesNearestRankPercentiles)
{
    for (auto duration = 1us; duration <= 10us; duration += 1us) {
        record(MethodId::MAP_BUFFER, duration);
    }

    const auto& command = summary("map_buffer");
    ASSERT_TRUE(command.p50_us);
    ASSERT_TRUE(command.p90_us);
    ASSERT_TRUE(command.p99_us);
    ASSERT_TRUE(command.max_us);
    EXPECT_EQ(*command.p50_us, 5U);
    EXPECT_EQ(*command.p90_us, 9U);
    EXPECT_EQ(*command.p99_us, 10U);
    EXPECT_EQ(*command.max_us, 10U);
}

TEST_F(CommandMetricsTest, KeepsMethodHistoriesIndependent)
{
    record(MethodId::MAP_BUFFER, 10us);
    record(MethodId::OPEN_DEVICE, 20us);

    const auto map_buffer = summary("map_buffer");
    const auto open_device = summary("open_device");
    EXPECT_EQ(map_buffer.samples, 1U);
    EXPECT_EQ(open_device.samples, 1U);
    ASSERT_TRUE(map_buffer.max_us);
    ASSERT_TRUE(open_device.max_us);
    EXPECT_EQ(*map_buffer.max_us, 10U);
    EXPECT_EQ(*open_device.max_us, 20U);
}

TEST_F(CommandMetricsTest, KeepsOnlyTheNewestSamplesAtCapacity)
{
    for (uint64_t duration = 1; duration <= 257; ++duration) {
        record(MethodId::MAP_BUFFER, std::chrono::microseconds { duration });
        clocks_.system += 1ms;
    }

    const auto& command = summary("map_buffer");
    EXPECT_EQ(command.samples, 256U);
    ASSERT_TRUE(command.p50_us);
    ASSERT_TRUE(command.max_us);
    EXPECT_EQ(*command.p50_us, 129U);
    EXPECT_EQ(*command.max_us, 257U);
    ASSERT_TRUE(command.oldest_completed_unix_ms);
    EXPECT_EQ(*command.oldest_completed_unix_ms, 1'700'000'000'001U);
}

TEST_F(CommandMetricsTest, AggregatesUnrecognizedMethodIds)
{
    record(static_cast<MethodId>(999), 10us);
    record(static_cast<MethodId>(1'000), 20us);

    const auto snapshot = metrics_.snapshot();
    ASSERT_EQ(snapshot.commands.size(), 11U);
    const auto& unknown = snapshot.commands.back();
    EXPECT_EQ(unknown.command, "unknown");
    EXPECT_EQ(unknown.samples, 2U);
    ASSERT_TRUE(unknown.p50_us);
    ASSERT_TRUE(unknown.max_us);
    EXPECT_EQ(*unknown.p50_us, 10U);
    EXPECT_EQ(*unknown.max_us, 20U);
}

TEST_F(CommandMetricsTest, IncludesFailedCommandsInLatencyPercentiles)
{
    record(MethodId::MAP_BUFFER, 1us);
    record(MethodId::MAP_BUFFER, 10us, StatusCode::INTERNAL);
    record(MethodId::MAP_BUFFER, 20us);

    const auto& command = summary("map_buffer");
    EXPECT_EQ(command.samples, 3U);
    EXPECT_EQ(command.errors, 1U);
    ASSERT_TRUE(command.p50_us);
    EXPECT_EQ(*command.p50_us, 10U);
}

TEST_F(CommandMetricsTest, RunsHeartbeatOnlyWhileCommandsAreActive)
{
    EXPECT_FALSE(timer_->deadline_);
    const auto started = clocks_.steady;

    const auto measurement = metrics_.begin(static_cast<uint32_t>(MethodId::MAP_BUFFER));

    ASSERT_TRUE(timer_->deadline_);
    EXPECT_EQ(*timer_->deadline_, started + CommandMetrics::HEARTBEAT_INTERVAL);

    metrics_.complete(measurement, StatusCode::OK);

    EXPECT_FALSE(timer_->deadline_);
    EXPECT_EQ(timer_->cancel_count_, 1U);
}

TEST_F(CommandMetricsTest, ReportsZeroLagBeforeHeartbeatDeadline)
{
    const auto measurement = metrics_.begin(static_cast<uint32_t>(MethodId::MAP_BUFFER));
    advance(500us);
    metrics_.complete(measurement, StatusCode::OK);

    const auto& command = summary("map_buffer");
    ASSERT_TRUE(command.event_loop_lag_p90_us);
    ASSERT_TRUE(command.event_loop_lag_max_us);
    EXPECT_EQ(*command.event_loop_lag_p90_us, 0U);
    EXPECT_EQ(*command.event_loop_lag_max_us, 0U);
}

TEST_F(CommandMetricsTest, RecordsOverdueHeartbeatWhenCommandCompletes)
{
    const auto measurement = metrics_.begin(static_cast<uint32_t>(MethodId::MAP_BUFFER));
    advance(5ms);
    metrics_.complete(measurement, StatusCode::OK);

    const auto& command = summary("map_buffer");
    ASSERT_TRUE(command.event_loop_lag_p90_us);
    ASSERT_TRUE(command.event_loop_lag_max_us);
    EXPECT_EQ(*command.event_loop_lag_p90_us, 4'000U);
    EXPECT_EQ(*command.event_loop_lag_max_us, 4'000U);
}

TEST_F(CommandMetricsTest, RearmsHeartbeatFromActualCallbackTime)
{
    const auto measurement = metrics_.begin(static_cast<uint32_t>(MethodId::MAP_BUFFER));
    advance(6ms);

    timer_->fire();

    ASSERT_TRUE(timer_->deadline_);
    EXPECT_EQ(*timer_->deadline_, clocks_.steady + CommandMetrics::HEARTBEAT_INTERVAL);
    metrics_.complete(measurement, StatusCode::OK);

    const auto& command = summary("map_buffer");
    ASSERT_TRUE(command.event_loop_lag_max_us);
    EXPECT_EQ(*command.event_loop_lag_max_us, 5'000U);
}

TEST_F(CommandMetricsTest, AppliesOverdueHeartbeatToEveryActiveCommand)
{
    const auto first = metrics_.begin(static_cast<uint32_t>(MethodId::MAP_BUFFER));
    const auto second = metrics_.begin(static_cast<uint32_t>(MethodId::MAP_BUFFER));
    advance(5ms);

    metrics_.complete(second, StatusCode::OK);
    metrics_.complete(first, StatusCode::OK);

    const auto& command = summary("map_buffer");
    EXPECT_EQ(command.samples, 2U);
    ASSERT_TRUE(command.event_loop_lag_p90_us);
    ASSERT_TRUE(command.event_loop_lag_max_us);
    EXPECT_EQ(*command.event_loop_lag_p90_us, 4'000U);
    EXPECT_EQ(*command.event_loop_lag_max_us, 4'000U);
}

TEST_F(CommandMetricsTest, HeartbeatCallbackUpdatesOverlappingCommands)
{
    const auto first = metrics_.begin(static_cast<uint32_t>(MethodId::MAP_BUFFER));
    const auto second = metrics_.begin(static_cast<uint32_t>(MethodId::OPEN_DEVICE));
    advance(3ms);

    timer_->fire();
    metrics_.complete(first, StatusCode::OK);
    metrics_.complete(second, StatusCode::OK);

    const auto map_buffer = summary("map_buffer");
    const auto open_device = summary("open_device");
    ASSERT_TRUE(map_buffer.event_loop_lag_max_us);
    ASSERT_TRUE(open_device.event_loop_lag_max_us);
    EXPECT_EQ(*map_buffer.event_loop_lag_max_us, 2'000U);
    EXPECT_EQ(*open_device.event_loop_lag_max_us, 2'000U);
}

TEST_F(CommandMetricsTest, DoesNotApplyAnOverdueHeartbeatToANewCommand)
{
    const auto first = metrics_.begin(static_cast<uint32_t>(MethodId::MAP_BUFFER));
    advance(5ms);

    const auto second = metrics_.begin(static_cast<uint32_t>(MethodId::OPEN_DEVICE));
    metrics_.complete(second, StatusCode::OK);
    metrics_.complete(first, StatusCode::OK);

    const auto map_buffer = summary("map_buffer");
    const auto open_device = summary("open_device");
    ASSERT_TRUE(map_buffer.event_loop_lag_max_us);
    ASSERT_TRUE(open_device.event_loop_lag_max_us);
    EXPECT_EQ(*map_buffer.event_loop_lag_max_us, 4'000U);
    EXPECT_EQ(*open_device.event_loop_lag_max_us, 0U);
}

} // anonymous namespace
