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

#include "render.hpp"

#include <google/protobuf/util/json_util.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <ctime>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace storage_lender::ctl {
namespace {

    using QuotaLimits = nvidia::storage_lender::ctl::v1::QuotaLimits;
    using QuotaUsage = nvidia::storage_lender::ctl::v1::QuotaUsage;
    using UsageTotals = nvidia::storage_lender::ctl::v1::UsageTotals;

    constexpr std::string_view ANSI_RESET = "\033[0m";
    constexpr std::string_view ANSI_HEADING = "\033[1m";
    constexpr std::string_view ANSI_AMBER = "\033[33m";
    constexpr std::string_view ANSI_RED = "\033[31m";

    enum class Resource {
        SESSIONS,
        TRANSFERRED_FDS,
        MAPPED_BUFFERS,
        MAX_BUFFER_BYTES,
        MAPPED_BYTES,
        DEVICE_HANDLES,
        COMPLETION_QUEUES,
        SUBMISSION_QUEUES,
    };

    struct ResourceInfo {
        Resource resource;
        std::string_view name;
    };

    constexpr std::array RESOURCES {
        ResourceInfo { Resource::SESSIONS, "sessions" },
        ResourceInfo { Resource::TRANSFERRED_FDS, "transferred_fds" },
        ResourceInfo { Resource::MAPPED_BUFFERS, "mapped_buffers" },
        ResourceInfo { Resource::MAX_BUFFER_BYTES, "max_buffer_bytes" },
        ResourceInfo { Resource::MAPPED_BYTES, "mapped_bytes" },
        ResourceInfo { Resource::DEVICE_HANDLES, "device_handles" },
        ResourceInfo { Resource::COMPLETION_QUEUES, "completion_queues" },
        ResourceInfo { Resource::SUBMISSION_QUEUES, "submission_queues" },
    };

    struct Row {
        std::string resource;
        std::string active;
        std::string orphan;
        std::string limit;
        bool reached_limit { false };
        bool orphan_nonzero { false };
    };

    std::string pad_left(std::string_view value, std::size_t width)
    {
        return std::string(width - value.size(), ' ') + std::string { value };
    }

    std::string pad_right(std::string_view value, std::size_t width)
    {
        return std::string { value } + std::string(width - value.size(), ' ');
    }

    std::string style(std::string_view value, std::string_view color, bool enabled)
    {
        if (!enabled) {
            return std::string { value };
        }
        return std::string { color } + std::string { value } + std::string { ANSI_RESET };
    }

    uint64_t usage_value(const QuotaUsage& usage, Resource resource)
    {
        switch (resource) {
        case Resource::SESSIONS:
            return usage.sessions();
        case Resource::TRANSFERRED_FDS:
            return usage.transferred_fds();
        case Resource::MAPPED_BUFFERS:
            return usage.mapped_buffers();
        case Resource::MAX_BUFFER_BYTES:
            return 0;
        case Resource::MAPPED_BYTES:
            return usage.mapped_bytes();
        case Resource::DEVICE_HANDLES:
            return usage.device_handles();
        case Resource::COMPLETION_QUEUES:
            return usage.completion_queues();
        case Resource::SUBMISSION_QUEUES:
            return usage.submission_queues();
        }
        return 0;
    }

    std::optional<uint64_t> limit_value(const QuotaLimits& limits, Resource resource)
    {
        switch (resource) {
        case Resource::SESSIONS:
            return limits.has_sessions() ? std::optional<uint64_t> { limits.sessions() } : std::nullopt;
        case Resource::TRANSFERRED_FDS:
            return limits.has_transferred_fds() ? std::optional<uint64_t> { limits.transferred_fds() } : std::nullopt;
        case Resource::MAPPED_BUFFERS:
            return limits.has_mapped_buffers() ? std::optional<uint64_t> { limits.mapped_buffers() } : std::nullopt;
        case Resource::MAX_BUFFER_BYTES:
            return limits.has_max_buffer_bytes() ? std::optional<uint64_t> { limits.max_buffer_bytes() } : std::nullopt;
        case Resource::MAPPED_BYTES:
            return limits.has_mapped_bytes() ? std::optional<uint64_t> { limits.mapped_bytes() } : std::nullopt;
        case Resource::DEVICE_HANDLES:
            return limits.has_device_handles() ? std::optional<uint64_t> { limits.device_handles() } : std::nullopt;
        case Resource::COMPLETION_QUEUES:
            return limits.has_completion_queues() ? std::optional<uint64_t> { limits.completion_queues() }
                                                  : std::nullopt;
        case Resource::SUBMISSION_QUEUES:
            return limits.has_submission_queues() ? std::optional<uint64_t> { limits.submission_queues() }
                                                  : std::nullopt;
        }
        return std::nullopt;
    }

    std::string mode_name(nvidia::storage_lender::ctl::v1::QuotaMode mode)
    {
        switch (mode) {
        case nvidia::storage_lender::ctl::v1::QUOTA_MODE_ENFORCED:
            return "enforced";
        case nvidia::storage_lender::ctl::v1::QUOTA_MODE_UNLIMITED:
            return "unlimited";
        default:
            return "unspecified";
        }
    }

    std::vector<Row> make_rows(const UsageTotals* usage, const QuotaLimits& limits)
    {
        std::vector<Row> rows;
        rows.reserve(RESOURCES.size());
        for (const auto& info : RESOURCES) {
            const auto limit = limit_value(limits, info.resource);
            Row row {
                .resource = std::string { info.name },
                .active = usage == nullptr || info.resource == Resource::MAX_BUFFER_BYTES
                    ? "-"
                    : std::to_string(usage_value(usage->active(), info.resource)),
                .orphan = usage == nullptr || info.resource == Resource::MAX_BUFFER_BYTES
                    ? "-"
                    : std::to_string(usage_value(usage->orphan(), info.resource)),
                .limit = limit ? std::to_string(*limit) : "unlimited",
            };
            if (usage != nullptr && info.resource != Resource::MAX_BUFFER_BYTES) {
                const auto active = usage_value(usage->active(), info.resource);
                const auto orphan = usage_value(usage->orphan(), info.resource);
                row.reached_limit = limit && active <= *limit && orphan == *limit - active;
                row.orphan_nonzero = orphan != 0;
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

    std::string render_table(std::string_view title, const UsageTotals* usage, const QuotaLimits& limits, bool color)
    {
        const auto rows = make_rows(usage, limits);
        std::size_t resource_width = std::string_view { "resource" }.size();
        std::size_t active_width = std::string_view { "active" }.size();
        std::size_t orphan_width = std::string_view { "orphan" }.size();
        std::size_t limit_width = std::string_view { "limit" }.size();
        for (const auto& row : rows) {
            resource_width = std::max(resource_width, row.resource.size());
            active_width = std::max(active_width, row.active.size());
            orphan_width = std::max(orphan_width, row.orphan.size());
            limit_width = std::max(limit_width, row.limit.size());
        }

        std::string output = style(title, ANSI_HEADING, color) + '\n';
        output += pad_right("resource", resource_width) + "  " + pad_left("active", active_width) + "  "
            + pad_left("orphan", orphan_width) + "  " + pad_left("limit", limit_width) + '\n';
        for (const auto& row : rows) {
            const auto active = pad_left(row.active, active_width);
            const auto orphan = pad_left(row.orphan, orphan_width);
            output += pad_right(row.resource, resource_width) + "  "
                + style(active, ANSI_RED, color && row.reached_limit) + "  "
                + style(orphan, row.reached_limit ? ANSI_RED : ANSI_AMBER,
                    color && (row.reached_limit || row.orphan_nonzero))
                + "  " + pad_left(row.limit, limit_width) + '\n';
        }
        return output;
    }

    using LatencyRow = std::array<std::string, 12>;

    constexpr std::array<std::string_view, 12> LATENCY_HEADERS {
        "command",
        "samples",
        "errors",
        "peak",
        "p50",
        "p90",
        "p99",
        "max",
        "lag_p90",
        "lag_max",
        "oldest",
        "newest",
    };

    std::string format_unix_milliseconds(uint64_t value)
    {
        const auto seconds_value = value / 1'000U;
        if (seconds_value > static_cast<uint64_t>(std::numeric_limits<std::time_t>::max())) {
            return "-";
        }

        const auto seconds = static_cast<std::time_t>(seconds_value);
        std::tm utc { };
        if (::gmtime_r(&seconds, &utc) == nullptr) {
            return "-";
        }
        std::array<char, 20> timestamp { };
        if (::strftime(timestamp.data(), timestamp.size(), "%Y-%m-%dT%H:%M:%S", &utc) == 0) {
            return "-";
        }
        const auto milliseconds = std::to_string(value % 1'000U);
        return std::string { timestamp.data() } + '.' + std::string(3 - milliseconds.size(), '0') + milliseconds + 'Z';
    }

    std::string optional_number(bool present, uint64_t value) { return present ? std::to_string(value) : "-"; }

    LatencyRow make_latency_row(const nvidia::storage_lender::ctl::v1::CommandLatencySummary& summary)
    {
        return LatencyRow {
            summary.command(),
            std::to_string(summary.samples()),
            std::to_string(summary.errors()),
            std::to_string(summary.peak_in_flight()),
            optional_number(summary.has_p50_us(), summary.p50_us()),
            optional_number(summary.has_p90_us(), summary.p90_us()),
            optional_number(summary.has_p99_us(), summary.p99_us()),
            optional_number(summary.has_max_us(), summary.max_us()),
            optional_number(summary.has_event_loop_lag_p90_us(), summary.event_loop_lag_p90_us()),
            optional_number(summary.has_event_loop_lag_max_us(), summary.event_loop_lag_max_us()),
            summary.has_oldest_completed_unix_ms() ? format_unix_milliseconds(summary.oldest_completed_unix_ms()) : "-",
            summary.has_newest_completed_unix_ms() ? format_unix_milliseconds(summary.newest_completed_unix_ms()) : "-",
        };
    }

    std::string render_latency_table(const nvidia::storage_lender::ctl::v1::GetCommandLatencyResponse& response)
    {
        std::vector<LatencyRow> rows;
        rows.reserve(response.commands_size());
        for (const auto& summary : response.commands()) {
            rows.push_back(make_latency_row(summary));
        }

        std::array<std::size_t, LATENCY_HEADERS.size()> widths { };
        for (auto index = std::size_t { }; index < widths.size(); ++index) {
            widths[index] = LATENCY_HEADERS[index].size();
        }
        for (const auto& row : rows) {
            for (auto index = std::size_t { }; index < widths.size(); ++index) {
                widths[index] = std::max(widths[index], row[index].size());
            }
        }

        std::string output;
        const auto append_row = [&output, &widths](const auto& row) {
            for (auto index = std::size_t { }; index < widths.size(); ++index) {
                if (index != 0) {
                    output += "  ";
                }
                output += index == 0 ? pad_right(row[index], widths[index]) : pad_left(row[index], widths[index]);
            }
            output += '\n';
        };
        append_row(LATENCY_HEADERS);
        for (const auto& row : rows) {
            append_row(row);
        }
        return output;
    }

    template <typename Message> std::expected<std::string, std::string> render_protobuf_json(const Message& message)
    {
        std::string output;
        const auto status = google::protobuf::util::MessageToJsonString(message, &output);
        if (!status.ok()) {
            return std::unexpected(status.ToString());
        }
        return output;
    }

} // anonymous namespace

bool color_enabled(ColorMode mode, bool stdout_is_tty, std::string_view term, bool no_color_set)
{
    switch (mode) {
    case ColorMode::AUTO:
        return stdout_is_tty && term != "dumb" && !no_color_set;
    case ColorMode::ALWAYS:
        return true;
    case ColorMode::NEVER:
        return false;
    }
    return false;
}

std::string render_human(const nvidia::storage_lender::ctl::v1::GetQuotaStateResponse& response, bool color)
{
    std::string output = style("quota state", ANSI_HEADING, color) + '\n';
    output += "generation: " + std::to_string(response.generation()) + '\n';
    output += "mode: " + mode_name(response.mode()) + "\n\n";
    output += render_table("global", &response.global_usage(), response.global_limits(), color);
    output += '\n';
    output += render_table("default principal limits", nullptr, response.default_principal_limits(), color);

    std::vector<const nvidia::storage_lender::ctl::v1::PrincipalQuotaState*> principals;
    principals.reserve(response.principals_size());
    for (const auto& principal : response.principals()) {
        principals.push_back(&principal);
    }
    std::ranges::sort(principals, { }, &nvidia::storage_lender::ctl::v1::PrincipalQuotaState::principal);
    for (const auto* principal : principals) {
        output += '\n';
        const auto title = "principal: " + principal->principal()
            + (principal->named_limit_override() ? " (named limits)" : " (default limits)");
        output += render_table(title, &principal->usage(), principal->limits(), color);
    }
    return output;
}

std::string render_human(const nvidia::storage_lender::ctl::v1::GetCommandLatencyResponse& response)
{
    std::string output = "command latency\n";
    output += "process started: " + format_unix_milliseconds(response.process_started_unix_ms()) + '\n';
    output += "window: last " + std::to_string(response.capacity_per_command()) + " per command\n";
    output += "units: microseconds\n\n";
    output += render_latency_table(response);
    return output;
}

std::expected<std::string, std::string> render_json(
    const nvidia::storage_lender::ctl::v1::GetQuotaStateResponse& response)
{
    return render_protobuf_json(response);
}

std::expected<std::string, std::string> render_json(
    const nvidia::storage_lender::ctl::v1::GetCommandLatencyResponse& response)
{
    return render_protobuf_json(response);
}

} // namespace storage_lender::ctl
