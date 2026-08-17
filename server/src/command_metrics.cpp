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

#include <boost/asio/steady_timer.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using MethodId = nvidia::storage_lender::v1::MethodId;
using StatusCode = nvidia::storage_lender::wire::v1::StatusCode;
using SteadyClock = std::chrono::steady_clock;
using SystemClock = std::chrono::system_clock;

enum class CommandOperation : uint8_t {
    TRANSFER_FD,
    MAP_BUFFER,
    UNMAP_BUFFER,
    OPEN_DEVICE,
    CLOSE_DEVICE,
    GET_DEVICE_INFO,
    CREATE_COMPLETION_QUEUE,
    DELETE_COMPLETION_QUEUE,
    CREATE_SUBMISSION_QUEUE,
    DELETE_SUBMISSION_QUEUE,
    UNKNOWN,
    COUNT,
};

constexpr auto OPERATION_COUNT = std::to_underlying(CommandOperation::COUNT);
constexpr auto KNOWN_OPERATION_COUNT = std::to_underlying(CommandOperation::UNKNOWN);

CommandOperation command_operation(uint32_t method)
{
    switch (static_cast<MethodId>(method)) {
    case MethodId::TRANSFER_FD:
        return CommandOperation::TRANSFER_FD;
    case MethodId::MAP_BUFFER:
        return CommandOperation::MAP_BUFFER;
    case MethodId::UNMAP_BUFFER:
        return CommandOperation::UNMAP_BUFFER;
    case MethodId::OPEN_DEVICE:
        return CommandOperation::OPEN_DEVICE;
    case MethodId::CLOSE_DEVICE:
        return CommandOperation::CLOSE_DEVICE;
    case MethodId::GET_DEVICE_INFO:
        return CommandOperation::GET_DEVICE_INFO;
    case MethodId::CREATE_COMPLETION_QUEUE:
        return CommandOperation::CREATE_COMPLETION_QUEUE;
    case MethodId::DELETE_COMPLETION_QUEUE:
        return CommandOperation::DELETE_COMPLETION_QUEUE;
    case MethodId::CREATE_SUBMISSION_QUEUE:
        return CommandOperation::CREATE_SUBMISSION_QUEUE;
    case MethodId::DELETE_SUBMISSION_QUEUE:
        return CommandOperation::DELETE_SUBMISSION_QUEUE;
    default:
        return CommandOperation::UNKNOWN;
    }
}

std::string_view command_name(CommandOperation operation)
{
    switch (operation) {
    case CommandOperation::TRANSFER_FD:
        return "transfer_fd";
    case CommandOperation::MAP_BUFFER:
        return "map_buffer";
    case CommandOperation::UNMAP_BUFFER:
        return "unmap_buffer";
    case CommandOperation::OPEN_DEVICE:
        return "open_device";
    case CommandOperation::CLOSE_DEVICE:
        return "close_device";
    case CommandOperation::GET_DEVICE_INFO:
        return "get_device_info";
    case CommandOperation::CREATE_COMPLETION_QUEUE:
        return "create_completion_queue";
    case CommandOperation::DELETE_COMPLETION_QUEUE:
        return "delete_completion_queue";
    case CommandOperation::CREATE_SUBMISSION_QUEUE:
        return "create_submission_queue";
    case CommandOperation::DELETE_SUBMISSION_QUEUE:
        return "delete_submission_queue";
    case CommandOperation::UNKNOWN:
        return "unknown";
    case CommandOperation::COUNT:
        break;
    }
    return "unknown";
}

uint64_t unix_milliseconds(SystemClock::time_point time)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count());
}

std::size_t nearest_rank_index(std::size_t count, uint32_t percentile)
{
    return ((count * percentile) + 99U) / 100U - 1U;
}

class AsioCommandMetricsTimer final : public CommandMetricsTimer {
public:
    explicit AsioCommandMetricsTimer(boost::asio::any_io_executor executor)
        : timer_ { std::move(executor) }
    {
    }

    void async_wait_until(TimePoint deadline, Handler handler) override
    {
        timer_.expires_at(deadline);
        timer_.async_wait(
            [handler = std::move(handler)](const boost::system::error_code& error) mutable { handler(error); });
    }

    void cancel() override
    {
        boost::system::error_code error;
        timer_.cancel(error);
    }

private:
    boost::asio::steady_timer timer_;
};

} // anonymous namespace

class CommandMetrics::Impl {
public:
    struct Observation {
        uint64_t duration_us { };
        bool error { };
        uint32_t in_flight { };
        uint64_t event_loop_lag_us { };
        uint64_t completed_unix_ms { };
    };

    class CommandWindow {
    public:
        void push(Observation observation)
        {
            observations_[next_] = observation;
            next_ = (next_ + 1) % observations_.size();
            size_ = std::min(size_ + 1, observations_.size());
        }

        bool empty() const { return size_ == 0; }

        std::size_t size() const { return size_; }

        const Observation& operator[](std::size_t index) const
        {
            const auto oldest = size_ == observations_.size() ? next_ : 0;
            return observations_[(oldest + index) % observations_.size()];
        }

    private:
        std::array<Observation, CommandMetrics::CAPACITY_PER_COMMAND> observations_ { };
        std::size_t size_ { };
        std::size_t next_ { };
    };

    struct ActiveMeasurement {
        uint64_t id;
        CommandOperation operation;
        SteadyClock::time_point started_at;
        uint32_t in_flight;
        uint64_t max_event_loop_lag_us { };
    };

    Impl(std::unique_ptr<CommandMetricsTimer> timer, SteadyNow steady_now, SystemNow system_now)
        : timer_ { std::move(timer) }
        , steady_now_ { std::move(steady_now) }
        , system_now_ { std::move(system_now) }
        , process_started_unix_ms_ { unix_milliseconds(system_now_()) }
    {
    }

    CommandMeasurement begin(uint32_t method)
    {
        const auto now = steady_now_();
        const auto sampled_overdue_heartbeat = record_overdue_heartbeat(now);
        const auto start_heartbeat = active_.empty();
        const auto measurement = CommandMeasurement { next_measurement_id_++ };
        active_.push_back(ActiveMeasurement {
            .id = measurement.id,
            .operation = command_operation(method),
            .started_at = now,
            .in_flight = static_cast<uint32_t>(active_.size() + 1),
        });
        if (start_heartbeat) {
            arm_heartbeat(now + CommandMetrics::HEARTBEAT_INTERVAL);
        } else if (sampled_overdue_heartbeat) {
            arm_heartbeat(now + CommandMetrics::HEARTBEAT_INTERVAL);
        }
        return measurement;
    }

    void complete(CommandMeasurement measurement, StatusCode status)
    {
        const auto active = std::ranges::find(active_, measurement.id, &ActiveMeasurement::id);
        if (active == active_.end()) {
            return;
        }

        const auto now = steady_now_();
        const auto sampled_overdue_heartbeat = record_overdue_heartbeat(now);
        windows_[std::to_underlying(active->operation)].push(Observation {
            .duration_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(now - active->started_at).count()),
            .error = status != StatusCode::OK,
            .in_flight = active->in_flight,
            .event_loop_lag_us = active->max_event_loop_lag_us,
            .completed_unix_ms = unix_milliseconds(system_now_()),
        });
        active_.erase(active);
        if (active_.empty()) {
            heartbeat_deadline_.reset();
            timer_->cancel();
        } else if (sampled_overdue_heartbeat) {
            arm_heartbeat(now + CommandMetrics::HEARTBEAT_INTERVAL);
        }
    }

    CommandMetricsSnapshot snapshot() const
    {
        CommandMetricsSnapshot result {
            .capacity_per_command = CommandMetrics::CAPACITY_PER_COMMAND,
            .process_started_unix_ms = process_started_unix_ms_,
        };
        result.commands.reserve(KNOWN_OPERATION_COUNT + 1);
        for (auto index = uint8_t { }; index < KNOWN_OPERATION_COUNT; ++index) {
            result.commands.push_back(summarize(static_cast<CommandOperation>(index)));
        }
        if (!windows_[std::to_underlying(CommandOperation::UNKNOWN)].empty()) {
            result.commands.push_back(summarize(CommandOperation::UNKNOWN));
        }
        return result;
    }

private:
    void arm_heartbeat(SteadyClock::time_point deadline)
    {
        heartbeat_deadline_ = deadline;
        timer_->async_wait_until(
            deadline, [this, deadline](boost::system::error_code error) { heartbeat(error, deadline); });
    }

    void heartbeat(boost::system::error_code error, SteadyClock::time_point deadline)
    {
        if (error || !heartbeat_deadline_ || *heartbeat_deadline_ != deadline) {
            return;
        }

        heartbeat_deadline_.reset();
        if (active_.empty()) {
            return;
        }

        const auto now = steady_now_();
        record_event_loop_lag(now, deadline);
        arm_heartbeat(now + CommandMetrics::HEARTBEAT_INTERVAL);
    }

    bool record_overdue_heartbeat(SteadyClock::time_point now)
    {
        if (!heartbeat_deadline_ || now <= *heartbeat_deadline_) {
            return false;
        }
        record_event_loop_lag(now, *heartbeat_deadline_);
        return true;
    }

    void record_event_loop_lag(SteadyClock::time_point now, SteadyClock::time_point expected)
    {
        const auto lag
            = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - expected).count());
        for (auto& measurement : active_) {
            measurement.max_event_loop_lag_us = std::max(measurement.max_event_loop_lag_us, lag);
        }
    }

    CommandLatencySummary summarize(CommandOperation operation) const
    {
        const auto& observations = windows_[std::to_underlying(operation)];
        CommandLatencySummary result {
            .command = command_name(operation),
            .samples = static_cast<uint32_t>(observations.size()),
        };
        if (observations.empty()) {
            return result;
        }

        std::vector<uint64_t> durations;
        std::vector<uint64_t> event_loop_lags;
        durations.reserve(observations.size());
        event_loop_lags.reserve(observations.size());
        for (auto index = std::size_t { }; index < observations.size(); ++index) {
            const auto& observation = observations[index];
            durations.push_back(observation.duration_us);
            event_loop_lags.push_back(observation.event_loop_lag_us);
            result.errors += observation.error ? 1U : 0U;
            result.peak_in_flight = std::max(result.peak_in_flight, observation.in_flight);
        }
        std::ranges::sort(durations);
        std::ranges::sort(event_loop_lags);

        result.p50_us = durations[nearest_rank_index(durations.size(), 50)];
        result.p90_us = durations[nearest_rank_index(durations.size(), 90)];
        result.p99_us = durations[nearest_rank_index(durations.size(), 99)];
        result.max_us = durations.back();
        result.event_loop_lag_p90_us = event_loop_lags[nearest_rank_index(event_loop_lags.size(), 90)];
        result.event_loop_lag_max_us = event_loop_lags.back();
        result.oldest_completed_unix_ms = observations[0].completed_unix_ms;
        result.newest_completed_unix_ms = observations[observations.size() - 1].completed_unix_ms;
        return result;
    }

    std::unique_ptr<CommandMetricsTimer> timer_;
    SteadyNow steady_now_;
    SystemNow system_now_;
    uint64_t process_started_unix_ms_;
    uint64_t next_measurement_id_ { 1 };
    std::array<CommandWindow, OPERATION_COUNT> windows_;
    std::vector<ActiveMeasurement> active_;
    std::optional<SteadyClock::time_point> heartbeat_deadline_;
};

CommandMetrics::CommandMetrics(boost::asio::any_io_executor executor)
    : CommandMetrics { std::make_unique<AsioCommandMetricsTimer>(std::move(executor)), SteadyClock::now,
        SystemClock::now }
{
}

CommandMetrics::CommandMetrics(std::unique_ptr<CommandMetricsTimer> timer, SteadyNow steady_now, SystemNow system_now)
    : impl_ { std::make_unique<Impl>(std::move(timer), std::move(steady_now), std::move(system_now)) }
{
}

CommandMetrics::~CommandMetrics() = default;

CommandMeasurement CommandMetrics::begin(uint32_t method) { return impl_->begin(method); }

void CommandMetrics::complete(CommandMeasurement measurement, StatusCode status)
{
    impl_->complete(measurement, status);
}

CommandMetricsSnapshot CommandMetrics::snapshot() const { return impl_->snapshot(); }
