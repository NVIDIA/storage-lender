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
#include <gtest/gtest.h>

#include <algorithm>
#include <regex>
#include <string>

namespace {

nvidia::storage_lender::ctl::v1::GetQuotaStateResponse quota_state()
{
    nvidia::storage_lender::ctl::v1::GetQuotaStateResponse response;
    response.set_generation(4);
    response.set_mode(nvidia::storage_lender::ctl::v1::QUOTA_MODE_ENFORCED);
    response.mutable_global_usage()->mutable_active()->set_sessions(1);
    response.mutable_global_usage()->mutable_orphan()->set_mapped_bytes(4096);
    response.mutable_global_limits()->set_sessions(2);
    response.mutable_default_principal_limits()->set_sessions(2);

    auto* storage = response.add_principals();
    storage->set_principal("storage");
    storage->mutable_usage()->mutable_active()->set_sessions(1);
    storage->mutable_usage()->mutable_orphan()->set_sessions(1);
    storage->mutable_limits()->set_sessions(2);
    storage->set_named_limit_override(true);

    auto* camera = response.add_principals();
    camera->set_principal("camera");
    camera->mutable_limits()->set_sessions(2);
    camera->set_named_limit_override(true);
    return response;
}

nvidia::storage_lender::ctl::v1::GetCommandLatencyResponse command_latency()
{
    nvidia::storage_lender::ctl::v1::GetCommandLatencyResponse response;
    response.set_capacity_per_command(256);
    response.set_process_started_unix_ms(1'700'000'000'123);

    auto* empty = response.add_commands();
    empty->set_command("transfer_fd");

    auto* populated = response.add_commands();
    populated->set_command("map_buffer");
    populated->set_samples(12);
    populated->set_errors(2);
    populated->set_peak_in_flight(3);
    populated->set_p50_us(10);
    populated->set_p90_us(20);
    populated->set_p99_us(30);
    populated->set_max_us(40);
    populated->set_event_loop_lag_p90_us(4);
    populated->set_event_loop_lag_max_us(6);
    populated->set_oldest_completed_unix_ms(1'700'000'001'001);
    populated->set_newest_completed_unix_ms(1'700'000'002'002);
    return response;
}

TEST(CtlRenderTest, AppliesTheColorPolicy)
{
    using storage_lender::ctl::color_enabled;
    using storage_lender::ctl::ColorMode;

    EXPECT_FALSE(color_enabled(ColorMode::AUTO, false, "xterm", false));
    EXPECT_FALSE(color_enabled(ColorMode::AUTO, true, "dumb", false));
    EXPECT_FALSE(color_enabled(ColorMode::AUTO, true, "xterm", true));
    EXPECT_TRUE(color_enabled(ColorMode::AUTO, true, "xterm", false));
    EXPECT_TRUE(color_enabled(ColorMode::ALWAYS, false, "dumb", true));
    EXPECT_FALSE(color_enabled(ColorMode::NEVER, true, "xterm", false));
}

TEST(CtlRenderTest, RendersDeterministicHumanQuotaState)
{
    const auto output = storage_lender::ctl::render_human(quota_state(), false);

    EXPECT_NE(output.find("quota state\ngeneration: 4\nmode: enforced\n\n"), std::string::npos);
    EXPECT_NE(output.find("global\nresource"), std::string::npos);
    EXPECT_NE(output.find("default principal limits\nresource"), std::string::npos);
    EXPECT_LT(output.find("principal: camera"), output.find("principal: storage"));
    EXPECT_LT(output.find("sessions"), output.find("transferred_fds"));
    EXPECT_LT(output.find("transferred_fds"), output.find("mapped_buffers"));
    EXPECT_LT(output.find("mapped_buffers"), output.find("max_buffer_bytes"));
    const auto max_buffer_bytes = output.find("max_buffer_bytes");
    ASSERT_NE(max_buffer_bytes, std::string::npos);
    EXPECT_NE(output.find("-       -", max_buffer_bytes), std::string::npos);
    EXPECT_EQ(output.find("\033["), std::string::npos);
}

TEST(CtlRenderTest, AppliesOnlySemanticColor)
{
    const auto output = storage_lender::ctl::render_human(quota_state(), true);

    EXPECT_NE(output.find("\033[33m  4096\033[0m"), std::string::npos);
    EXPECT_NE(output.find("\033[31m     1\033[0m"), std::string::npos);
    EXPECT_EQ(output.find("\033[33m     1\033[0m"), std::string::npos);
}

TEST(CtlRenderTest, RendersProtobufJson)
{
    const auto json = storage_lender::ctl::render_json(quota_state());

    ASSERT_TRUE(json.has_value());
    EXPECT_NE(json->find("\"generation\":\"4\""), std::string::npos);
    EXPECT_EQ(json->find("\033["), std::string::npos);

    nvidia::storage_lender::ctl::v1::GetQuotaStateResponse parsed;
    const auto status = google::protobuf::util::JsonStringToMessage(*json, &parsed);
    ASSERT_TRUE(status.ok()) << status.ToString();
    EXPECT_EQ(parsed.generation(), 4U);
}

TEST(CtlRenderTest, RendersDeterministicHumanCommandLatency)
{
    const auto response = command_latency();
    const auto output = storage_lender::ctl::render_human(response);

    EXPECT_EQ(output, storage_lender::ctl::render_human(response));
    EXPECT_TRUE(output.starts_with("command latency\nprocess started: 2023-11-14T22:13:20.123Z\n"
                                   "window: last 256 per command\nunits: microseconds\n\n"));
    EXPECT_LT(output.find("transfer_fd"), output.find("map_buffer"));

    const auto map_start = output.find("map_buffer");
    ASSERT_NE(map_start, std::string::npos);
    const auto map_end = output.find('\n', map_start);
    const auto map_line = output.substr(map_start, map_end - map_start);
    const std::regex expected {
        R"(map_buffer\s+12\s+2\s+3\s+10\s+20\s+30\s+40\s+4\s+6\s+2023-11-14T22:13:21\.001Z\s+2023-11-14T22:13:22\.002Z)"
    };
    EXPECT_TRUE(std::regex_match(map_line, expected)) << map_line;
}

TEST(CtlRenderTest, RendersAbsentLatencyValuesAsDashes)
{
    const auto output = storage_lender::ctl::render_human(command_latency());
    const auto row_start = output.find("transfer_fd");
    ASSERT_NE(row_start, std::string::npos);
    const auto row_end = output.find('\n', row_start);
    const auto row = output.substr(row_start, row_end - row_start);

    EXPECT_EQ(std::ranges::count(row, '-'), 8);
}

TEST(CtlRenderTest, RendersCommandLatencyProtobufJson)
{
    const auto json = storage_lender::ctl::render_json(command_latency());

    ASSERT_TRUE(json.has_value());
    EXPECT_NE(json->find("\"capacityPerCommand\":256"), std::string::npos);
    EXPECT_EQ(json->find("\033["), std::string::npos);

    nvidia::storage_lender::ctl::v1::GetCommandLatencyResponse parsed;
    const auto status = google::protobuf::util::JsonStringToMessage(*json, &parsed);
    ASSERT_TRUE(status.ok()) << status.ToString();
    ASSERT_EQ(parsed.commands_size(), 2);
    EXPECT_EQ(parsed.commands(1).p99_us(), 30U);
}

} // anonymous namespace
