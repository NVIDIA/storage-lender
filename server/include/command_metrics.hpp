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

#include "client.pb.h"
#include "wire.pb.h"

#include <boost/asio/any_io_executor.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

class CommandMetricsTimer {
public:
    using Handler = std::function<void(boost::system::error_code)>;
    using TimePoint = std::chrono::steady_clock::time_point;

    virtual ~CommandMetricsTimer() = default;

    virtual void async_wait_until(TimePoint deadline, Handler handler) = 0;
    virtual void cancel() = 0;
};

struct CommandMeasurement {
    uint64_t id;
};

struct CommandLatencySummary {
    std::string_view command { };
    uint32_t samples { };
    uint32_t errors { };
    uint32_t peak_in_flight { };
    std::optional<uint64_t> p50_us { };
    std::optional<uint64_t> p90_us { };
    std::optional<uint64_t> p99_us { };
    std::optional<uint64_t> max_us { };
    std::optional<uint64_t> event_loop_lag_p90_us { };
    std::optional<uint64_t> event_loop_lag_max_us { };
    std::optional<uint64_t> oldest_completed_unix_ms { };
    std::optional<uint64_t> newest_completed_unix_ms { };
};

struct CommandMetricsSnapshot {
    uint32_t capacity_per_command { };
    uint64_t process_started_unix_ms { };
    std::vector<CommandLatencySummary> commands { };
};

class CommandMetrics {
public:
    using SteadyNow = std::function<std::chrono::steady_clock::time_point()>;
    using SystemNow = std::function<std::chrono::system_clock::time_point()>;

    static constexpr uint32_t CAPACITY_PER_COMMAND = 256;
    static constexpr std::chrono::milliseconds HEARTBEAT_INTERVAL { 1 };

    explicit CommandMetrics(boost::asio::any_io_executor executor);
    CommandMetrics(std::unique_ptr<CommandMetricsTimer> timer, SteadyNow steady_now, SystemNow system_now);
    ~CommandMetrics();

    CommandMetrics(const CommandMetrics&) = delete;
    CommandMetrics& operator=(const CommandMetrics&) = delete;

    CommandMeasurement begin(uint32_t method);
    void complete(CommandMeasurement measurement, nvidia::storage_lender::wire::v1::StatusCode status);
    CommandMetricsSnapshot snapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
