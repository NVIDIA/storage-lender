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

#include "ctl_test_client.hpp"
#include "protocol.hpp"
#include "server_test_harness.hpp"
#include "test_dma_buffer.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

using ::testing::_;
using ::testing::Return;

namespace {

const nvidia::storage_lender::ctl::v1::PrincipalQuotaState* find_principal(
    const nvidia::storage_lender::ctl::v1::GetQuotaStateResponse& response, std::string_view principal)
{
    for (const auto& state : response.principals()) {
        if (state.principal() == principal) {
            return &state;
        }
    }
    return nullptr;
}

const nvidia::storage_lender::ctl::v1::CommandLatencySummary* find_command(
    const nvidia::storage_lender::ctl::v1::GetCommandLatencyResponse& response, std::string_view command)
{
    for (const auto& summary : response.commands()) {
        if (summary.command() == command) {
            return &summary;
        }
    }
    return nullptr;
}

} // anonymous namespace

class ServerManagementIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        auto started = harness_.start();
        ASSERT_TRUE(started.has_value()) << started.error().message();
    }

    std::expected<CtlTestClient, int> connect_ctl() const { return CtlTestClient::connect(harness_.ctl_socket_path()); }

    ServerTestHarness harness_;
};

TEST_F(ServerManagementIntegrationTest, LatencySnapshotStartsEmpty)
{
    auto client = connect_ctl();
    ASSERT_TRUE(client.has_value()) << "ctl connect failed (errno=" << client.error() << ')';

    auto response = client->call_get_command_latency();
    ASSERT_TRUE(response.has_value()) << "ctl request failed (errno=" << response.error() << ')';

    EXPECT_EQ(response->capacity_per_command(), 256U);
    EXPECT_GT(response->process_started_unix_ms(), 0U);
    constexpr std::array<std::string_view, 10> EXPECTED_COMMANDS {
        "transfer_fd",
        "map_buffer",
        "unmap_buffer",
        "open_device",
        "close_device",
        "get_device_info",
        "create_completion_queue",
        "delete_completion_queue",
        "create_submission_queue",
        "delete_submission_queue",
    };
    ASSERT_EQ(response->commands_size(), static_cast<int>(EXPECTED_COMMANDS.size()));
    for (auto index = std::size_t { }; index < EXPECTED_COMMANDS.size(); ++index) {
        const auto& command = response->commands(static_cast<int>(index));
        EXPECT_EQ(command.command(), EXPECTED_COMMANDS[index]);
        EXPECT_EQ(command.samples(), 0U);
        EXPECT_EQ(command.errors(), 0U);
        EXPECT_EQ(command.peak_in_flight(), 0U);
        EXPECT_FALSE(command.has_p50_us());
        EXPECT_FALSE(command.has_event_loop_lag_max_us());
        EXPECT_FALSE(command.has_oldest_completed_unix_ms());
    }
}

TEST_F(ServerManagementIntegrationTest, CompletedApiCommandAppearsInLatencySnapshot)
{
    auto api_client = harness_.connect_protocol();
    ASSERT_TRUE(api_client.has_value()) << "API connect failed (errno=" << api_client.error() << ')';
    auto buffer = make_sealed_memfd(4096);
    ASSERT_TRUE(buffer.has_value()) << "memfd creation failed (errno=" << buffer.error() << ')';
    auto fd_id = api_client->transfer_fd(buffer->get());
    ASSERT_TRUE(fd_id.has_value()) << "transfer failed (errno=" << fd_id.error() << ')';

    auto client = connect_ctl();
    ASSERT_TRUE(client.has_value()) << "ctl connect failed (errno=" << client.error() << ')';
    auto response = client->call_get_command_latency();
    ASSERT_TRUE(response.has_value()) << "ctl request failed (errno=" << response.error() << ')';

    const auto* command = find_command(*response, "transfer_fd");
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->samples(), 1U);
    EXPECT_EQ(command->errors(), 0U);
    EXPECT_EQ(command->peak_in_flight(), 1U);
    EXPECT_TRUE(command->has_p50_us());
    EXPECT_TRUE(command->has_p90_us());
    EXPECT_TRUE(command->has_p99_us());
    EXPECT_TRUE(command->has_max_us());
    EXPECT_TRUE(command->has_event_loop_lag_p90_us());
    EXPECT_TRUE(command->has_event_loop_lag_max_us());
    EXPECT_GT(command->oldest_completed_unix_ms(), 0U);
    EXPECT_GE(command->newest_completed_unix_ms(), command->oldest_completed_unix_ms());
}

TEST_F(ServerManagementIntegrationTest, ErrorAndUnknownApiMethodsAreCounted)
{
    auto api_client = harness_.connect_protocol();
    ASSERT_TRUE(api_client.has_value()) << "API connect failed (errno=" << api_client.error() << ')';

    nvidia::storage_lender::v1::DeviceInfoRequest request;
    request.set_device_id(999);
    auto known_error = api_client->call(MethodId::GET_DEVICE_INFO, request);
    ASSERT_TRUE(known_error.has_value()) << "API request failed (errno=" << known_error.error() << ')';
    EXPECT_EQ(known_error->status_code(), StatusCode::NOT_FOUND);

    ASSERT_TRUE(api_client->send_request(999));
    ProtocolTestClient::Response unknown_error;
    ASSERT_TRUE(api_client->recv_response(unknown_error));
    EXPECT_EQ(unknown_error.status_code(), StatusCode::INVALID_ARGUMENT);

    auto client = connect_ctl();
    ASSERT_TRUE(client.has_value()) << "ctl connect failed (errno=" << client.error() << ')';
    auto response = client->call_get_command_latency();
    ASSERT_TRUE(response.has_value()) << "ctl request failed (errno=" << response.error() << ')';

    const auto* get_device_info = find_command(*response, "get_device_info");
    ASSERT_NE(get_device_info, nullptr);
    EXPECT_EQ(get_device_info->samples(), 1U);
    EXPECT_EQ(get_device_info->errors(), 1U);
    const auto* unknown = find_command(*response, "unknown");
    ASSERT_NE(unknown, nullptr);
    EXPECT_EQ(unknown->samples(), 1U);
    EXPECT_EQ(unknown->errors(), 1U);
}

TEST_F(ServerManagementIntegrationTest, LatencyQueryDoesNotCountItself)
{
    auto first_client = connect_ctl();
    ASSERT_TRUE(first_client.has_value()) << "ctl connect failed (errno=" << first_client.error() << ')';
    auto first = first_client->call_get_command_latency();
    ASSERT_TRUE(first.has_value()) << "ctl request failed (errno=" << first.error() << ')';

    auto second_client = connect_ctl();
    ASSERT_TRUE(second_client.has_value()) << "ctl connect failed (errno=" << second_client.error() << ')';
    auto second = second_client->call_get_command_latency();
    ASSERT_TRUE(second.has_value()) << "ctl request failed (errno=" << second.error() << ')';

    ASSERT_EQ(first->commands_size(), second->commands_size());
    for (auto index = 0; index < first->commands_size(); ++index) {
        EXPECT_EQ(first->commands(index).command(), second->commands(index).command());
        EXPECT_EQ(first->commands(index).samples(), second->commands(index).samples());
        EXPECT_EQ(first->commands(index).errors(), second->commands(index).errors());
    }
}

TEST_F(ServerManagementIntegrationTest, EmptySnapshotIncludesGlobalAndDefaultLimits)
{
    auto client = connect_ctl();
    ASSERT_TRUE(client.has_value()) << "ctl connect failed (errno=" << client.error() << ')';

    auto response = client->call_get_quota_state();
    ASSERT_TRUE(response.has_value()) << "ctl request failed (errno=" << response.error() << ')';

    EXPECT_EQ(response->generation(), 1U);
    EXPECT_EQ(response->mode(), nvidia::storage_lender::ctl::v1::QUOTA_MODE_UNLIMITED);
    EXPECT_TRUE(response->has_global_usage());
    EXPECT_TRUE(response->has_global_limits());
    EXPECT_TRUE(response->has_default_principal_limits());
    EXPECT_TRUE(response->principals().empty());
}

TEST_F(ServerManagementIntegrationTest, SnapshotReportsActivePrincipalUsage)
{
    auto api_client = harness_.connect_protocol();
    ASSERT_TRUE(api_client.has_value()) << "API connect failed (errno=" << api_client.error() << ')';
    ASSERT_TRUE(harness_.wait_for_usage([](const UsageSnapshot& usage) {
        const auto principal = usage.principal_usage.find("default");
        return principal != usage.principal_usage.end() && principal->second.active.sessions == 1;
    }));

    auto client = connect_ctl();
    ASSERT_TRUE(client.has_value()) << "ctl connect failed (errno=" << client.error() << ')';
    auto response = client->call_get_quota_state();
    ASSERT_TRUE(response.has_value()) << "ctl request failed (errno=" << response.error() << ')';

    const auto* principal = find_principal(*response, "default");
    ASSERT_NE(principal, nullptr);
    EXPECT_EQ(principal->usage().active().sessions(), 1U);
    EXPECT_EQ(principal->usage().orphan().sessions(), 0U);
}

TEST_F(ServerManagementIntegrationTest, SnapshotReportsOrphanUsage)
{
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), mem_unregister_dma_buf(_, 4096)).WillOnce(Return(std::unexpected(EIO)));

    {
        auto api_client = harness_.connect_protocol();
        ASSERT_TRUE(api_client.has_value()) << "API connect failed (errno=" << api_client.error() << ')';
        auto buffer = make_sealed_memfd(4096);
        ASSERT_TRUE(buffer.has_value()) << "memfd creation failed (errno=" << buffer.error() << ')';
        auto fd_id = api_client->transfer_fd(buffer->get());
        ASSERT_TRUE(fd_id.has_value()) << "transfer failed (errno=" << fd_id.error() << ')';
        auto iova = api_client->map_buffer(*fd_id, 4096);
        ASSERT_TRUE(iova.has_value()) << "map failed (errno=" << iova.error() << ')';
    }

    ASSERT_TRUE(harness_.wait_for_usage([](const UsageSnapshot& usage) {
        const auto principal = usage.principal_usage.find("default");
        return principal != usage.principal_usage.end() && principal->second.orphan.mapped_buffers == 1
            && principal->second.orphan.mapped_bytes == 4096;
    }));

    auto client = connect_ctl();
    ASSERT_TRUE(client.has_value()) << "ctl connect failed (errno=" << client.error() << ')';
    auto response = client->call_get_quota_state();
    ASSERT_TRUE(response.has_value()) << "ctl request failed (errno=" << response.error() << ')';

    const auto* principal = find_principal(*response, "default");
    ASSERT_NE(principal, nullptr);
    EXPECT_EQ(principal->usage().orphan().mapped_buffers(), 1U);
    EXPECT_EQ(principal->usage().orphan().mapped_bytes(), 4096U);
}

TEST(ServerManagementSnapshotTest, SnapshotIncludesNamedUnusedPrincipalInLexicalOrder)
{
    QuotaPolicy policy;
    policy.mode = QuotaMode::ENFORCED;
    auto storage_limits = QuotaLimits { };
    storage_limits.sessions = 3;
    policy.principals.emplace("storage", storage_limits);
    auto camera_limits = QuotaLimits { };
    camera_limits.sessions = 2;
    policy.principals.emplace("camera", camera_limits);
    ServerConfig config {
        .api_socket = { },
        .ctl_socket = { },
        .device_policy = test_device_policy(),
        .quota_policy = std::move(policy),
        .principal_resolver = PrincipalResolver { { }, { } },
    };
    ServerTestHarness harness;
    auto started = harness.start(std::move(config), default_test_peer_credentials(), { }, "management-order");
    ASSERT_TRUE(started.has_value()) << started.error().message();

    auto client = CtlTestClient::connect(harness.ctl_socket_path());
    ASSERT_TRUE(client.has_value()) << "ctl connect failed (errno=" << client.error() << ')';
    auto response = client->call_get_quota_state();
    ASSERT_TRUE(response.has_value()) << "ctl request failed (errno=" << response.error() << ')';

    ASSERT_EQ(response->principals_size(), 2);
    EXPECT_EQ(response->principals(0).principal(), "camera");
    EXPECT_TRUE(response->principals(0).named_limit_override());
    EXPECT_EQ(response->principals(1).principal(), "storage");
    EXPECT_TRUE(response->principals(1).named_limit_override());
}

TEST_F(ServerManagementIntegrationTest, UnknownMethodReturnsInvalidArgumentThenConnectionCloses)
{
    auto client = connect_ctl();
    ASSERT_TRUE(client.has_value()) << "ctl connect failed (errno=" << client.error() << ')';
    ASSERT_TRUE(client->send_request(999));

    CtlTestClient::Response response;
    ASSERT_TRUE(client->recv_response(response));
    EXPECT_EQ(response.status_code(), nvidia::storage_lender::wire::v1::INVALID_ARGUMENT);
    EXPECT_FALSE(client->recv_response(response));
}

TEST_F(ServerManagementIntegrationTest, NonemptyQuotaRequestReturnsInvalidArgumentThenConnectionCloses)
{
    auto client = connect_ctl();
    ASSERT_TRUE(client.has_value()) << "ctl connect failed (errno=" << client.error() << ')';
    ASSERT_TRUE(client->send_request(
        static_cast<uint32_t>(nvidia::storage_lender::ctl::v1::GET_QUOTA_STATE), "unexpected payload"));

    CtlTestClient::Response response;
    ASSERT_TRUE(client->recv_response(response));
    EXPECT_EQ(response.status_code(), nvidia::storage_lender::wire::v1::INVALID_ARGUMENT);
    EXPECT_FALSE(client->recv_response(response));
}

TEST_F(ServerManagementIntegrationTest, NonemptyLatencyRequestReturnsInvalidArgumentThenConnectionCloses)
{
    auto client = connect_ctl();
    ASSERT_TRUE(client.has_value()) << "ctl connect failed (errno=" << client.error() << ')';
    ASSERT_TRUE(client->send_request(
        static_cast<uint32_t>(nvidia::storage_lender::ctl::v1::GET_COMMAND_LATENCY), "unexpected payload"));

    CtlTestClient::Response response;
    ASSERT_TRUE(client->recv_response(response));
    EXPECT_EQ(response.status_code(), nvidia::storage_lender::wire::v1::INVALID_ARGUMENT);
    EXPECT_FALSE(client->recv_response(response));
}

TEST_F(ServerManagementIntegrationTest, MalformedEnvelopeClosesConnection)
{
    auto client = connect_ctl();
    ASSERT_TRUE(client.has_value()) << "ctl connect failed (errno=" << client.error() << ')';
    ASSERT_TRUE(client->send_raw_request_frame(std::string(1, '\x80')));

    CtlTestClient::Response response;
    ASSERT_TRUE(client->recv_response(response));
    EXPECT_EQ(response.status_code(), nvidia::storage_lender::wire::v1::INVALID_ARGUMENT);
    EXPECT_FALSE(client->recv_response(response));
}

TEST_F(ServerManagementIntegrationTest, SecondRequestOnOneConnectionIsNotServed)
{
    auto client = connect_ctl();
    ASSERT_TRUE(client.has_value()) << "ctl connect failed (errno=" << client.error() << ')';
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(nvidia::storage_lender::ctl::v1::GET_QUOTA_STATE)));

    CtlTestClient::Response response;
    ASSERT_TRUE(client->recv_response(response));
    if (client->send_request(static_cast<uint32_t>(nvidia::storage_lender::ctl::v1::GET_QUOTA_STATE))) {
        EXPECT_FALSE(client->recv_response(response));
    }
}
