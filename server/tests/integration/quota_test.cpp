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

#include "protocol.hpp"
#include "protocol_test_client.hpp"
#include "server_test_harness.hpp"
#include "test_dma_buffer.hpp"

#include "client.pb.h"
#include "wire.pb.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

using ::testing::_;
using ::testing::Return;

class ServerQuotaIntegrationTest : public ::testing::Test {
protected:
    void start(ServerConfig config, PeerCredentialReader reader)
    {
        auto started = harness_.start(std::move(config), std::move(reader), { }, "quota");
        ASSERT_TRUE(started.has_value()) << started.error().message();
    }

    UsageSnapshot snapshot() { return harness_.usage_snapshot(); }

    bool wait_for_usage(const std::function<bool(const UsageSnapshot&)>& predicate)
    {
        return harness_.wait_for_usage(predicate);
    }

    static void expect_alive(ProtocolTestClient& client)
    {
        ASSERT_TRUE(client.send_request(999));
        nvidia::storage_lender::wire::v1::Response response;
        ASSERT_TRUE(client.recv_response(response));
        EXPECT_EQ(response.status_code(), static_cast<int32_t>(StatusCode::INVALID_ARGUMENT));
    }

    static nvidia::storage_lender::wire::v1::Response call(
        ProtocolTestClient& client, MethodId method, const google::protobuf::MessageLite& request)
    {
        auto response = client.call(method, request);
        EXPECT_TRUE(response.has_value());
        return response ? std::move(*response) : nvidia::storage_lender::wire::v1::Response { };
    }

    static uint32_t transfer_fd(ProtocolTestClient& client, int fd)
    {
        auto fd_id = client.transfer_fd(fd);
        EXPECT_TRUE(fd_id.has_value());
        return fd_id.value_or(0);
    }

    static uint64_t map_buffer(ProtocolTestClient& client, uint32_t fd_id, uint64_t size)
    {
        auto iova = client.map_buffer(fd_id, size);
        EXPECT_TRUE(iova.has_value());
        return iova.value_or(0);
    }

    static uint32_t open_device(ProtocolTestClient& client, std::string_view pci_address)
    {
        auto device_id = client.open_shared_device(pci_address);
        EXPECT_TRUE(device_id.has_value());
        return device_id.value_or(0);
    }

    ServerTestHarness harness_;
    void* const CTRLR = reinterpret_cast<void*>(0xCAFE);
};

TEST_F(ServerQuotaIntegrationTest, SessionLimitClosesNewConnection)
{
    QuotaPolicy policy;
    policy.mode = QuotaMode::ENFORCED;
    policy.global.sessions = 1;
    start(ServerConfig { .api_socket = { },
              .device_policy = test_device_policy(),
              .quota_policy = std::move(policy),
              .principal_resolver = PrincipalResolver { { }, { } } },
        sequential_test_peer_credentials({ PeerCredentials { .pid = 1, .uid = 1000, .gid = 1000 } }));

    auto first = harness_.connect_protocol();

    ASSERT_TRUE(first.has_value()) << "protocol connect failed (errno=" << first.error() << ')';
    expect_alive(*first);
    auto denied = harness_.connect_protocol();
    ASSERT_TRUE(denied.has_value()) << "protocol connect failed (errno=" << denied.error() << ')';
    nvidia::storage_lender::wire::v1::Response response;
    EXPECT_FALSE(denied->recv_response(response));
}

TEST_F(ServerQuotaIntegrationTest, SessionsForOnePrincipalShareLimit)
{
    QuotaPolicy policy;
    policy.mode = QuotaMode::ENFORCED;
    policy.default_principal.sessions = 1;
    start(ServerConfig { .api_socket = { },
              .device_policy = test_device_policy(),
              .quota_policy = std::move(policy),
              .principal_resolver = PrincipalResolver { { }, { } } },
        sequential_test_peer_credentials({ PeerCredentials { .pid = 1, .uid = 1000, .gid = 1000 } }));

    auto first = harness_.connect_protocol();

    ASSERT_TRUE(first.has_value()) << "protocol connect failed (errno=" << first.error() << ')';
    expect_alive(*first);
    auto denied = harness_.connect_protocol();
    ASSERT_TRUE(denied.has_value()) << "protocol connect failed (errno=" << denied.error() << ')';
    nvidia::storage_lender::wire::v1::Response response;
    EXPECT_FALSE(denied->recv_response(response));
}

TEST_F(ServerQuotaIntegrationTest, DifferentPrincipalsHaveIndependentLimits)
{
    QuotaLimits one_session;
    one_session.sessions = 1;
    QuotaPolicy policy;
    policy.mode = QuotaMode::ENFORCED;
    policy.principals = { { "camera", one_session }, { "storage", one_session } };
    PrincipalResolver resolver { { { 1001, "camera" }, { 1002, "storage" } }, { } };
    start(
        ServerConfig {
            .api_socket = { },
            .device_policy = test_device_policy(),
            .quota_policy = std::move(policy),
            .principal_resolver = std::move(resolver),
        },
        sequential_test_peer_credentials({ PeerCredentials { .pid = 1, .uid = 1001, .gid = 1000 },
            PeerCredentials { .pid = 2, .uid = 1002, .gid = 1000 } }));

    auto camera = harness_.connect_protocol();

    ASSERT_TRUE(camera.has_value()) << "protocol connect failed (errno=" << camera.error() << ')';
    auto storage = harness_.connect_protocol();
    ASSERT_TRUE(storage.has_value()) << "protocol connect failed (errno=" << storage.error() << ')';
    expect_alive(*camera);
    expect_alive(*storage);
}

TEST_F(ServerQuotaIntegrationTest, GlobalLimitSpansPrincipals)
{
    QuotaPolicy policy;
    policy.mode = QuotaMode::ENFORCED;
    policy.global.sessions = 1;
    policy.principals = { { "camera", QuotaLimits { } }, { "storage", QuotaLimits { } } };
    PrincipalResolver resolver { { { 1001, "camera" }, { 1002, "storage" } }, { } };
    start(
        ServerConfig {
            .api_socket = { },
            .device_policy = test_device_policy(),
            .quota_policy = std::move(policy),
            .principal_resolver = std::move(resolver),
        },
        sequential_test_peer_credentials({ PeerCredentials { .pid = 1, .uid = 1001, .gid = 1000 },
            PeerCredentials { .pid = 2, .uid = 1002, .gid = 1000 } }));

    auto first = harness_.connect_protocol();

    ASSERT_TRUE(first.has_value()) << "protocol connect failed (errno=" << first.error() << ')';
    expect_alive(*first);
    auto denied = harness_.connect_protocol();
    ASSERT_TRUE(denied.has_value()) << "protocol connect failed (errno=" << denied.error() << ')';
    nvidia::storage_lender::wire::v1::Response response;
    EXPECT_FALSE(denied->recv_response(response));
}

TEST_F(ServerQuotaIntegrationTest, UnmatchedPeerUsesDefaultLimits)
{
    QuotaLimits camera_limits;
    camera_limits.sessions = 1;
    QuotaPolicy policy;
    policy.mode = QuotaMode::ENFORCED;
    policy.default_principal.sessions = 0;
    policy.principals = { { "camera", camera_limits } };
    PrincipalResolver resolver { { { 1001, "camera" } }, { } };
    start(
        ServerConfig {
            .api_socket = { },
            .device_policy = test_device_policy(),
            .quota_policy = std::move(policy),
            .principal_resolver = std::move(resolver),
        },
        sequential_test_peer_credentials({ PeerCredentials { .pid = 1, .uid = 9999, .gid = 9999 } }));

    auto denied = harness_.connect_protocol();

    ASSERT_TRUE(denied.has_value()) << "protocol connect failed (errno=" << denied.error() << ')';
    nvidia::storage_lender::wire::v1::Response response;
    EXPECT_FALSE(denied->recv_response(response));
}

TEST_F(ServerQuotaIntegrationTest, DeniedFdIsConsumedClosedAndNextRequestParses)
{
    QuotaPolicy policy;
    policy.mode = QuotaMode::ENFORCED;
    policy.default_principal.transferred_fds = 0;
    start(ServerConfig { .api_socket = { },
              .device_policy = test_device_policy(),
              .quota_policy = std::move(policy),
              .principal_resolver = PrincipalResolver { { }, { } } },
        sequential_test_peer_credentials({ PeerCredentials { .pid = 1, .uid = 1000, .gid = 1000 } }));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    auto fd = make_sealed_memfd(4096);
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD)));
    ASSERT_TRUE(client->send_fd_scm_rights(fd->get()));

    nvidia::storage_lender::wire::v1::Response response;
    ASSERT_TRUE(client->recv_response(response));
    EXPECT_EQ(response.status_code(), static_cast<int32_t>(StatusCode::RESOURCE_EXHAUSTED));
    expect_alive(*client);
    EXPECT_EQ(snapshot().global_usage.active.transferred_fds, 0u);
}

TEST_F(ServerQuotaIntegrationTest, BufferCountSizeAndBytesReturnResourceExhausted)
{
    QuotaLimits count_limits;
    count_limits.mapped_buffers = 0;
    QuotaLimits size_limits;
    size_limits.max_buffer_bytes = 4095;
    QuotaLimits bytes_limits;
    bytes_limits.mapped_bytes = 4095;

    QuotaPolicy policy;
    policy.mode = QuotaMode::ENFORCED;
    policy.principals = {
        { "count", count_limits },
        { "size", size_limits },
        { "bytes", bytes_limits },
    };
    PrincipalResolver resolver {
        { { 1001, "count" }, { 1002, "size" }, { 1003, "bytes" } },
        { },
    };
    start(
        ServerConfig {
            .api_socket = { },
            .device_policy = test_device_policy(),
            .quota_policy = std::move(policy),
            .principal_resolver = std::move(resolver),
        },
        sequential_test_peer_credentials({ PeerCredentials { .pid = 1, .uid = 1001, .gid = 1000 },
            PeerCredentials { .pid = 2, .uid = 1002, .gid = 1000 },
            PeerCredentials { .pid = 3, .uid = 1003, .gid = 1000 } }));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, _)).Times(0);

    std::vector<ProtocolTestClient> clients;
    for (int i = 0; i < 3; ++i) {
        auto client = harness_.connect_protocol();
        ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
        clients.push_back(std::move(*client));
        auto fd = make_sealed_memfd(4096);
        ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
        const auto fd_id = transfer_fd(clients.back(), fd->get());

        nvidia::storage_lender::v1::MapBufferRequest request;
        request.set_fd_id(fd_id);
        request.set_size(4096);
        auto response = call(clients.back(), MethodId::MAP_BUFFER, request);
        EXPECT_EQ(response.status_code(), static_cast<int32_t>(StatusCode::RESOURCE_EXHAUSTED));
    }
}

TEST_F(ServerQuotaIntegrationTest, RepeatedSharedOpensConsumeDistinctDeviceQuota)
{
    QuotaPolicy policy;
    policy.mode = QuotaMode::ENFORCED;
    policy.default_principal.device_handles = 1;
    start(ServerConfig { .api_socket = { },
              .device_policy = test_device_policy(),
              .quota_policy = std::move(policy),
              .principal_resolver = PrincipalResolver { { }, { } } },
        sequential_test_peer_credentials({ PeerCredentials { .pid = 1, .uid = 1000, .gid = 1000 } }));
    EXPECT_CALL(harness_.backend(), nvme_connect("0000:01:00.0", _)).WillOnce(Return(CTRLR));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    EXPECT_NE(open_device(*client, "0000:01:00.0"), 0u);

    nvidia::storage_lender::v1::OpenDeviceRequest request;
    request.set_pci_address("0000:01:00.0");
    request.set_open_mode(nvidia::storage_lender::v1::SHARED);
    auto response = call(*client, MethodId::OPEN_DEVICE, request);
    EXPECT_EQ(response.status_code(), static_cast<int32_t>(StatusCode::RESOURCE_EXHAUSTED));
}

TEST_F(ServerQuotaIntegrationTest, FailedFinalDeviceDetachRetainsQuota)
{
    QuotaPolicy policy;
    policy.mode = QuotaMode::ENFORCED;
    policy.default_principal.device_handles = 1;
    start(ServerConfig { .api_socket = { },
              .device_policy = test_device_policy(),
              .quota_policy = std::move(policy),
              .principal_resolver = PrincipalResolver { { }, { } } },
        sequential_test_peer_credentials({ PeerCredentials { .pid = 1, .uid = 1000, .gid = 1000 } }));
    EXPECT_CALL(harness_.backend(), nvme_connect("0000:01:00.0", _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), nvme_detach(CTRLR))
        .WillOnce(Return(std::unexpected(-1)))
        .WillOnce(Return(std::expected<void, int> { }));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    const auto device_id = open_device(*client, "0000:01:00.0");

    nvidia::storage_lender::v1::CloseDeviceRequest close_request;
    close_request.set_device_id(device_id);
    auto response = call(*client, MethodId::CLOSE_DEVICE, close_request);
    EXPECT_EQ(response.status_code(), static_cast<int32_t>(StatusCode::INTERNAL));

    nvidia::storage_lender::v1::OpenDeviceRequest open_request;
    open_request.set_pci_address("0000:01:00.0");
    open_request.set_open_mode(nvidia::storage_lender::v1::SHARED);
    response = call(*client, MethodId::OPEN_DEVICE, open_request);
    EXPECT_EQ(response.status_code(), static_cast<int32_t>(StatusCode::RESOURCE_EXHAUSTED));

    response = call(*client, MethodId::CLOSE_DEVICE, close_request);
    EXPECT_EQ(response.status_code(), static_cast<int32_t>(StatusCode::OK));
}

TEST_F(ServerQuotaIntegrationTest, CqAndSqLimitsRollbackOnBackendFailure)
{
    QuotaPolicy policy;
    policy.mode = QuotaMode::ENFORCED;
    policy.default_principal.completion_queues = 1;
    policy.default_principal.submission_queues = 1;
    start(ServerConfig { .api_socket = { },
              .device_policy = test_device_policy(),
              .quota_policy = std::move(policy),
              .principal_resolver = PrincipalResolver { { }, { } } },
        sequential_test_peer_credentials({ PeerCredentials { .pid = 1, .uid = 1000, .gid = 1000 } }));

    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _))
        .WillOnce(Return(std::unexpected(-1)))
        .WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(harness_.backend(), create_sq(CTRLR, _, _, 1, _))
        .WillOnce(Return(std::unexpected(-1)))
        .WillOnce(Return(NvmeBackend::QueueInfo { 2, 0 }));
    EXPECT_CALL(harness_.backend(), delete_sq(CTRLR, 2, _)).WillOnce(Return(std::expected<void, int> { }));
    EXPECT_CALL(harness_.backend(), delete_cq(CTRLR, 1, _)).WillOnce(Return(std::expected<void, int> { }));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    const auto device_id = open_device(*client, "0000:01:00.0");
    auto fd = make_sealed_memfd(4096);
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
    const auto fd_id = transfer_fd(*client, fd->get());
    const auto iova = map_buffer(*client, fd_id, 4096);

    nvidia::storage_lender::v1::CreateCompletionQueueRequest cq_request;
    cq_request.set_device_id(device_id);
    cq_request.set_iova(iova);
    cq_request.set_queue_size(64);
    EXPECT_EQ(call(*client, MethodId::CREATE_COMPLETION_QUEUE, cq_request).status_code(),
        static_cast<int32_t>(StatusCode::INTERNAL));
    auto cq_response = call(*client, MethodId::CREATE_COMPLETION_QUEUE, cq_request);
    ASSERT_EQ(cq_response.status_code(), static_cast<int32_t>(StatusCode::OK));
    nvidia::storage_lender::v1::CreateCompletionQueueResponse cq;
    ASSERT_TRUE(cq.ParseFromString(cq_response.payload()));

    nvidia::storage_lender::v1::CreateSubmissionQueueRequest sq_request;
    sq_request.set_device_id(device_id);
    sq_request.set_cq_id(cq.cq_id());
    sq_request.set_iova(iova);
    sq_request.set_queue_size(64);
    EXPECT_EQ(call(*client, MethodId::CREATE_SUBMISSION_QUEUE, sq_request).status_code(),
        static_cast<int32_t>(StatusCode::INTERNAL));
    auto sq_response = call(*client, MethodId::CREATE_SUBMISSION_QUEUE, sq_request);
    ASSERT_EQ(sq_response.status_code(), static_cast<int32_t>(StatusCode::OK));
    nvidia::storage_lender::v1::CreateSubmissionQueueResponse sq;
    ASSERT_TRUE(sq.ParseFromString(sq_response.payload()));

    nvidia::storage_lender::v1::DeleteSubmissionQueueRequest delete_sq;
    delete_sq.set_sq_id(sq.sq_id());
    EXPECT_EQ(call(*client, MethodId::DELETE_SUBMISSION_QUEUE, delete_sq).status_code(),
        static_cast<int32_t>(StatusCode::OK));
    nvidia::storage_lender::v1::DeleteCompletionQueueRequest delete_cq;
    delete_cq.set_cq_id(cq.cq_id());
    EXPECT_EQ(call(*client, MethodId::DELETE_COMPLETION_QUEUE, delete_cq).status_code(),
        static_cast<int32_t>(StatusCode::OK));
}

TEST_F(ServerQuotaIntegrationTest, ExplicitDeleteFailureRetainsQuota)
{
    QuotaPolicy policy;
    policy.mode = QuotaMode::ENFORCED;
    policy.default_principal.completion_queues = 1;
    start(ServerConfig { .api_socket = { },
              .device_policy = test_device_policy(),
              .quota_policy = std::move(policy),
              .principal_resolver = PrincipalResolver { { }, { } } },
        sequential_test_peer_credentials({ PeerCredentials { .pid = 1, .uid = 1000, .gid = 1000 } }));

    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(harness_.backend(), delete_cq(CTRLR, 1, _))
        .WillOnce(Return(std::unexpected(-1)))
        .WillOnce(Return(std::expected<void, int> { }));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    const auto device_id = open_device(*client, "0000:01:00.0");
    auto fd = make_sealed_memfd(4096);
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
    const auto fd_id = transfer_fd(*client, fd->get());
    const auto iova = map_buffer(*client, fd_id, 4096);

    nvidia::storage_lender::v1::CreateCompletionQueueRequest create;
    create.set_device_id(device_id);
    create.set_iova(iova);
    create.set_queue_size(64);
    auto created = call(*client, MethodId::CREATE_COMPLETION_QUEUE, create);
    nvidia::storage_lender::v1::CreateCompletionQueueResponse cq;
    ASSERT_TRUE(cq.ParseFromString(created.payload()));

    nvidia::storage_lender::v1::DeleteCompletionQueueRequest remove;
    remove.set_cq_id(cq.cq_id());
    EXPECT_EQ(call(*client, MethodId::DELETE_COMPLETION_QUEUE, remove).status_code(),
        static_cast<int32_t>(StatusCode::INTERNAL));
    EXPECT_EQ(call(*client, MethodId::CREATE_COMPLETION_QUEUE, create).status_code(),
        static_cast<int32_t>(StatusCode::RESOURCE_EXHAUSTED));
}

TEST_F(ServerQuotaIntegrationTest, DisconnectCleanupReleasesConfirmedResources)
{
    start(unlimited_server_config(),
        sequential_test_peer_credentials({ PeerCredentials { .pid = 1, .uid = 1000, .gid = 1000 } }));
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), nvme_detach(CTRLR)).WillOnce(Return(std::expected<void, int> { }));

    {
        auto client = harness_.connect_protocol();
        ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
        EXPECT_NE(open_device(*client, "0000:01:00.0"), 0u);
    }

    EXPECT_TRUE(wait_for_usage([](const UsageSnapshot& usage) {
        return usage.global_usage.active == QuotaAmounts { } && usage.global_usage.orphan == QuotaAmounts { };
    }));
}

TEST_F(ServerQuotaIntegrationTest, DisconnectCleanupRetainsOrphanCharges)
{
    start(unlimited_server_config(),
        sequential_test_peer_credentials({ PeerCredentials { .pid = 1, .uid = 1000, .gid = 1000 } }));
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), nvme_detach(CTRLR))
        .WillOnce(Return(std::unexpected(-1)))
        .WillRepeatedly(Return(std::expected<void, int> { }));

    {
        auto client = harness_.connect_protocol();
        ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
        EXPECT_NE(open_device(*client, "0000:01:00.0"), 0u);
    }

    EXPECT_TRUE(wait_for_usage([](const UsageSnapshot& usage) {
        return usage.global_usage.active.sessions == 0 && usage.global_usage.active.device_handles == 0
            && usage.global_usage.orphan.device_handles == 1;
    }));
}
