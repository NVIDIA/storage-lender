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
#include "server_test_harness.hpp"
#include "test_dma_buffer.hpp"

#include "client.pb.h"
#include "wire.pb.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>

using ::testing::_;
using ::testing::Return;

class ServerQueueIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        auto started = harness_.start();
        ASSERT_TRUE(started.has_value()) << started.error().message();
    }

    ServerTestHarness harness_;
    void* const CTRLR = reinterpret_cast<void*>(0xCAFE);
};

TEST_F(ServerQueueIntegrationTest, CreateCompletionQueueInvalidWithoutBufIova)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    {
        nvidia::storage_lender::v1::OpenDeviceRequest req;
        req.set_pci_address("0000:01:00.0");
        req.set_open_mode(nvidia::storage_lender::v1::SHARED);
        client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    nvidia::storage_lender::v1::CreateCompletionQueueRequest req;
    req.set_device_id(1);
    req.set_iova(0xDEAD);
    req.set_queue_size(64);

    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::CREATE_COMPLETION_QUEUE), req));

    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::INVALID_ARGUMENT));
}

TEST_F(ServerQueueIntegrationTest, DeleteCompletionQueueNotFoundOnBadId)
{
    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    nvidia::storage_lender::v1::DeleteCompletionQueueRequest req;
    req.set_cq_id(999);

    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::DELETE_COMPLETION_QUEUE), req));

    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::NOT_FOUND));
}

TEST_F(ServerQueueIntegrationTest, CreateAndDeleteCompletionQueue)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x7000 }));
    EXPECT_CALL(harness_.backend(), delete_cq(CTRLR, 1, _)).WillOnce(Return(std::expected<void, int> { }));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    {
        nvidia::storage_lender::v1::OpenDeviceRequest req;
        req.set_pci_address("0000:01:00.0");
        req.set_open_mode(nvidia::storage_lender::v1::SHARED);
        client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD));
    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    client->send_fd_scm_rights(memfd->get());
    nvidia::storage_lender::wire::v1::Response fd_resp;
    client->recv_response(fd_resp);
    nvidia::storage_lender::v1::TransferFdResponse fd_msg;
    fd_msg.ParseFromString(fd_resp.payload());

    nvidia::storage_lender::v1::MapBufferRequest map_req;
    map_req.set_fd_id(fd_msg.fd_id());
    map_req.set_size(4096);
    client->send_request(static_cast<uint32_t>(MethodId::MAP_BUFFER), map_req);
    nvidia::storage_lender::wire::v1::Response map_resp;
    client->recv_response(map_resp);
    nvidia::storage_lender::v1::MapBufferResponse map_msg;
    map_msg.ParseFromString(map_resp.payload());

    nvidia::storage_lender::v1::CreateCompletionQueueRequest cq_req;
    cq_req.set_device_id(1);
    cq_req.set_iova(map_msg.iova());
    cq_req.set_queue_size(64);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::CREATE_COMPLETION_QUEUE), cq_req));
    nvidia::storage_lender::wire::v1::Response cq_resp;
    ASSERT_TRUE(client->recv_response(cq_resp));
    ASSERT_EQ(cq_resp.status_code(), static_cast<int32_t>(StatusCode::OK));
    nvidia::storage_lender::v1::CreateCompletionQueueResponse cq_msg;
    ASSERT_TRUE(cq_msg.ParseFromString(cq_resp.payload()));
    EXPECT_GE(cq_msg.cq_id(), 1u);

    nvidia::storage_lender::v1::DeleteCompletionQueueRequest del_req;
    del_req.set_cq_id(cq_msg.cq_id());
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::DELETE_COMPLETION_QUEUE), del_req));
    nvidia::storage_lender::wire::v1::Response del_resp;
    ASSERT_TRUE(client->recv_response(del_resp));
    ASSERT_EQ(del_resp.status_code(), static_cast<int32_t>(StatusCode::OK));
}

TEST_F(ServerQueueIntegrationTest, DeleteCqFailsWhenSqsStillExist)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x7000 }));
    EXPECT_CALL(harness_.backend(), create_sq(CTRLR, _, _, 1, _))
        .WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x8000 }));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    {
        nvidia::storage_lender::v1::OpenDeviceRequest req;
        req.set_pci_address("0000:01:00.0");
        req.set_open_mode(nvidia::storage_lender::v1::SHARED);
        client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD));
    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    client->send_fd_scm_rights(memfd->get());
    nvidia::storage_lender::wire::v1::Response fd_resp;
    client->recv_response(fd_resp);
    nvidia::storage_lender::v1::TransferFdResponse fd_msg;
    fd_msg.ParseFromString(fd_resp.payload());

    nvidia::storage_lender::v1::MapBufferRequest map_req;
    map_req.set_fd_id(fd_msg.fd_id());
    map_req.set_size(4096);
    client->send_request(static_cast<uint32_t>(MethodId::MAP_BUFFER), map_req);
    nvidia::storage_lender::wire::v1::Response map_resp;
    client->recv_response(map_resp);
    nvidia::storage_lender::v1::MapBufferResponse map_msg;
    map_msg.ParseFromString(map_resp.payload());

    {
        nvidia::storage_lender::v1::CreateCompletionQueueRequest req;
        req.set_device_id(1);
        req.set_iova(map_msg.iova());
        req.set_queue_size(64);
        client->send_request(static_cast<uint32_t>(MethodId::CREATE_COMPLETION_QUEUE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
        nvidia::storage_lender::v1::CreateCompletionQueueResponse cq_msg;
        cq_msg.ParseFromString(resp.payload());

        nvidia::storage_lender::v1::CreateSubmissionQueueRequest sq_req;
        sq_req.set_device_id(1);
        sq_req.set_cq_id(cq_msg.cq_id());
        sq_req.set_iova(map_msg.iova());
        sq_req.set_queue_size(64);
        client->send_request(static_cast<uint32_t>(MethodId::CREATE_SUBMISSION_QUEUE), sq_req);
        nvidia::storage_lender::wire::v1::Response sq_resp;
        client->recv_response(sq_resp);

        nvidia::storage_lender::v1::DeleteCompletionQueueRequest del_req;
        del_req.set_cq_id(cq_msg.cq_id());
        ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::DELETE_COMPLETION_QUEUE), del_req));
        nvidia::storage_lender::wire::v1::Response del_resp;
        ASSERT_TRUE(client->recv_response(del_resp));
        EXPECT_EQ(del_resp.status_code(), static_cast<int32_t>(StatusCode::FAILED_PRECONDITION));
    }
}

TEST_F(ServerQueueIntegrationTest, CreateAndDeleteSubmissionQueue)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x7000 }));
    EXPECT_CALL(harness_.backend(), create_sq(CTRLR, _, _, 1, _))
        .WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x8000 }));
    EXPECT_CALL(harness_.backend(), delete_sq(CTRLR, 1, _)).WillOnce(Return(std::expected<void, int> { }));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    {
        nvidia::storage_lender::v1::OpenDeviceRequest req;
        req.set_pci_address("0000:01:00.0");
        req.set_open_mode(nvidia::storage_lender::v1::SHARED);
        client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD));
    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    client->send_fd_scm_rights(memfd->get());
    nvidia::storage_lender::wire::v1::Response fd_resp;
    client->recv_response(fd_resp);
    nvidia::storage_lender::v1::TransferFdResponse fd_msg;
    fd_msg.ParseFromString(fd_resp.payload());

    nvidia::storage_lender::v1::MapBufferRequest map_req;
    map_req.set_fd_id(fd_msg.fd_id());
    map_req.set_size(4096);
    client->send_request(static_cast<uint32_t>(MethodId::MAP_BUFFER), map_req);
    nvidia::storage_lender::wire::v1::Response map_resp;
    client->recv_response(map_resp);
    nvidia::storage_lender::v1::MapBufferResponse map_msg;
    map_msg.ParseFromString(map_resp.payload());

    nvidia::storage_lender::v1::CreateCompletionQueueRequest cq_req;
    cq_req.set_device_id(1);
    cq_req.set_iova(map_msg.iova());
    cq_req.set_queue_size(64);
    client->send_request(static_cast<uint32_t>(MethodId::CREATE_COMPLETION_QUEUE), cq_req);
    nvidia::storage_lender::wire::v1::Response cq_resp;
    client->recv_response(cq_resp);
    nvidia::storage_lender::v1::CreateCompletionQueueResponse cq_msg;
    cq_msg.ParseFromString(cq_resp.payload());

    nvidia::storage_lender::v1::CreateSubmissionQueueRequest sq_req;
    sq_req.set_device_id(1);
    sq_req.set_cq_id(cq_msg.cq_id());
    sq_req.set_iova(map_msg.iova());
    sq_req.set_queue_size(64);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::CREATE_SUBMISSION_QUEUE), sq_req));
    nvidia::storage_lender::wire::v1::Response sq_resp;
    ASSERT_TRUE(client->recv_response(sq_resp));
    ASSERT_EQ(sq_resp.status_code(), static_cast<int32_t>(StatusCode::OK));
    nvidia::storage_lender::v1::CreateSubmissionQueueResponse sq_msg;
    ASSERT_TRUE(sq_msg.ParseFromString(sq_resp.payload()));
    EXPECT_GE(sq_msg.sq_id(), 1u);

    nvidia::storage_lender::v1::DeleteSubmissionQueueRequest del_req;
    del_req.set_sq_id(sq_msg.sq_id());
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::DELETE_SUBMISSION_QUEUE), del_req));
    nvidia::storage_lender::wire::v1::Response del_resp;
    ASSERT_TRUE(client->recv_response(del_resp));
    ASSERT_EQ(del_resp.status_code(), static_cast<int32_t>(StatusCode::OK));
}

TEST_F(ServerQueueIntegrationTest, CreateCqFailsWithUnknownDeviceId)
{
    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD));
    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    client->send_fd_scm_rights(memfd->get());
    nvidia::storage_lender::wire::v1::Response fd_resp;
    client->recv_response(fd_resp);
    nvidia::storage_lender::v1::TransferFdResponse fd_msg;
    fd_msg.ParseFromString(fd_resp.payload());

    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    nvidia::storage_lender::v1::MapBufferRequest map_req;
    map_req.set_fd_id(fd_msg.fd_id());
    map_req.set_size(4096);
    client->send_request(static_cast<uint32_t>(MethodId::MAP_BUFFER), map_req);
    nvidia::storage_lender::wire::v1::Response map_resp;
    client->recv_response(map_resp);
    nvidia::storage_lender::v1::MapBufferResponse map_msg;
    map_msg.ParseFromString(map_resp.payload());

    nvidia::storage_lender::v1::CreateCompletionQueueRequest cq_req;
    cq_req.set_device_id(999);
    cq_req.set_iova(map_msg.iova());
    cq_req.set_queue_size(64);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::CREATE_COMPLETION_QUEUE), cq_req));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::NOT_FOUND));
}

// CREATE_COMPLETION_QUEUE: backend create_cq fails → INTERNAL.
TEST_F(ServerQueueIntegrationTest, CreateCqFailsWhenBackendFails)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(std::unexpected(-1)));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    {
        nvidia::storage_lender::v1::OpenDeviceRequest req;
        req.set_pci_address("0000:01:00.0");
        req.set_open_mode(nvidia::storage_lender::v1::SHARED);
        client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD));
    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    client->send_fd_scm_rights(memfd->get());
    nvidia::storage_lender::wire::v1::Response fd_resp;
    client->recv_response(fd_resp);
    nvidia::storage_lender::v1::TransferFdResponse fd_msg;
    fd_msg.ParseFromString(fd_resp.payload());

    nvidia::storage_lender::v1::MapBufferRequest map_req;
    map_req.set_fd_id(fd_msg.fd_id());
    map_req.set_size(4096);
    client->send_request(static_cast<uint32_t>(MethodId::MAP_BUFFER), map_req);
    nvidia::storage_lender::wire::v1::Response map_resp;
    client->recv_response(map_resp);
    nvidia::storage_lender::v1::MapBufferResponse map_msg;
    map_msg.ParseFromString(map_resp.payload());

    nvidia::storage_lender::v1::CreateCompletionQueueRequest cq_req;
    cq_req.set_device_id(1);
    cq_req.set_iova(map_msg.iova());
    cq_req.set_queue_size(64);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::CREATE_COMPLETION_QUEUE), cq_req));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::INTERNAL));
}

// DELETE_COMPLETION_QUEUE: backend delete_cq fails → INTERNAL.
TEST_F(ServerQueueIntegrationTest, DeleteCqFailsWhenBackendFails)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x7000 }));
    EXPECT_CALL(harness_.backend(), delete_cq(CTRLR, 1, _))
        .WillOnce(Return(std::unexpected(-1))) // explicit DELETE_SUBMISSION_QUEUE request
        .WillRepeatedly(Return(std::expected<void, int> { })); // cleanup_session retry on disconnect

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    {
        nvidia::storage_lender::v1::OpenDeviceRequest req;
        req.set_pci_address("0000:01:00.0");
        req.set_open_mode(nvidia::storage_lender::v1::SHARED);
        client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD));
    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    client->send_fd_scm_rights(memfd->get());
    nvidia::storage_lender::wire::v1::Response fd_resp;
    client->recv_response(fd_resp);
    nvidia::storage_lender::v1::TransferFdResponse fd_msg;
    fd_msg.ParseFromString(fd_resp.payload());

    nvidia::storage_lender::v1::MapBufferRequest map_req;
    map_req.set_fd_id(fd_msg.fd_id());
    map_req.set_size(4096);
    client->send_request(static_cast<uint32_t>(MethodId::MAP_BUFFER), map_req);
    nvidia::storage_lender::wire::v1::Response map_resp;
    client->recv_response(map_resp);
    nvidia::storage_lender::v1::MapBufferResponse map_msg;
    map_msg.ParseFromString(map_resp.payload());

    {
        nvidia::storage_lender::v1::CreateCompletionQueueRequest req;
        req.set_device_id(1);
        req.set_iova(map_msg.iova());
        req.set_queue_size(64);
        client->send_request(static_cast<uint32_t>(MethodId::CREATE_COMPLETION_QUEUE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    nvidia::storage_lender::v1::DeleteCompletionQueueRequest del_req;
    del_req.set_cq_id(1);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::DELETE_COMPLETION_QUEUE), del_req));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::INTERNAL));
}

// CREATE_SUBMISSION_QUEUE: iova not mapped → INVALID_ARGUMENT.
TEST_F(ServerQueueIntegrationTest, CreateSqFailsWithUnknownIova)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x7000 }));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    {
        nvidia::storage_lender::v1::OpenDeviceRequest req;
        req.set_pci_address("0000:01:00.0");
        req.set_open_mode(nvidia::storage_lender::v1::SHARED);
        client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD));
    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    client->send_fd_scm_rights(memfd->get());
    nvidia::storage_lender::wire::v1::Response fd_resp;
    client->recv_response(fd_resp);
    nvidia::storage_lender::v1::TransferFdResponse fd_msg;
    fd_msg.ParseFromString(fd_resp.payload());

    nvidia::storage_lender::v1::MapBufferRequest map_req;
    map_req.set_fd_id(fd_msg.fd_id());
    map_req.set_size(4096);
    client->send_request(static_cast<uint32_t>(MethodId::MAP_BUFFER), map_req);
    nvidia::storage_lender::wire::v1::Response map_resp;
    client->recv_response(map_resp);

    nvidia::storage_lender::v1::MapBufferResponse map_msg;
    map_msg.ParseFromString(map_resp.payload());

    {
        nvidia::storage_lender::v1::CreateCompletionQueueRequest req;
        req.set_device_id(1);
        req.set_iova(map_msg.iova());
        req.set_queue_size(64);
        client->send_request(static_cast<uint32_t>(MethodId::CREATE_COMPLETION_QUEUE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    nvidia::storage_lender::v1::CreateSubmissionQueueRequest sq_req;
    sq_req.set_device_id(1);
    sq_req.set_cq_id(1);
    sq_req.set_iova(0xDEADBEEF);
    sq_req.set_queue_size(64);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::CREATE_SUBMISSION_QUEUE), sq_req));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::INVALID_ARGUMENT));
}

// CREATE_SUBMISSION_QUEUE: cq_id not found → NOT_FOUND.
TEST_F(ServerQueueIntegrationTest, CreateSqFailsWhenCqNotFound)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    {
        nvidia::storage_lender::v1::OpenDeviceRequest req;
        req.set_pci_address("0000:01:00.0");
        req.set_open_mode(nvidia::storage_lender::v1::SHARED);
        client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD));
    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    client->send_fd_scm_rights(memfd->get());
    nvidia::storage_lender::wire::v1::Response fd_resp;
    client->recv_response(fd_resp);
    nvidia::storage_lender::v1::TransferFdResponse fd_msg;
    fd_msg.ParseFromString(fd_resp.payload());

    nvidia::storage_lender::v1::MapBufferRequest map_req;
    map_req.set_fd_id(fd_msg.fd_id());
    map_req.set_size(4096);
    client->send_request(static_cast<uint32_t>(MethodId::MAP_BUFFER), map_req);
    nvidia::storage_lender::wire::v1::Response map_resp;
    client->recv_response(map_resp);
    nvidia::storage_lender::v1::MapBufferResponse map_msg;
    map_msg.ParseFromString(map_resp.payload());

    nvidia::storage_lender::v1::CreateSubmissionQueueRequest sq_req;
    sq_req.set_device_id(1);
    sq_req.set_cq_id(999);
    sq_req.set_iova(map_msg.iova());
    sq_req.set_queue_size(64);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::CREATE_SUBMISSION_QUEUE), sq_req));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::NOT_FOUND));
}

// DELETE_SUBMISSION_QUEUE: sq_id not found → NOT_FOUND.
TEST_F(ServerQueueIntegrationTest, DeleteSqFailsWhenNotFound)
{
    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    nvidia::storage_lender::v1::DeleteSubmissionQueueRequest req;
    req.set_sq_id(999);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::DELETE_SUBMISSION_QUEUE), req));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::NOT_FOUND));
}

TEST_F(ServerQueueIntegrationTest, DeleteCqFailsWhenGetCtrlrFails)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x7000 }));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    {
        nvidia::storage_lender::v1::OpenDeviceRequest req;
        req.set_pci_address("0000:01:00.0");
        req.set_open_mode(nvidia::storage_lender::v1::SHARED);
        client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD));
    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    client->send_fd_scm_rights(memfd->get());
    nvidia::storage_lender::wire::v1::Response fd_resp;
    client->recv_response(fd_resp);
    nvidia::storage_lender::v1::TransferFdResponse fd_msg;
    fd_msg.ParseFromString(fd_resp.payload());

    nvidia::storage_lender::v1::MapBufferRequest map_req;
    map_req.set_fd_id(fd_msg.fd_id());
    map_req.set_size(4096);
    client->send_request(static_cast<uint32_t>(MethodId::MAP_BUFFER), map_req);
    nvidia::storage_lender::wire::v1::Response map_resp;
    client->recv_response(map_resp);
    nvidia::storage_lender::v1::MapBufferResponse map_msg;
    map_msg.ParseFromString(map_resp.payload());

    {
        nvidia::storage_lender::v1::CreateCompletionQueueRequest req;
        req.set_device_id(1);
        req.set_iova(map_msg.iova());
        req.set_queue_size(64);
        client->send_request(static_cast<uint32_t>(MethodId::CREATE_COMPLETION_QUEUE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    ASSERT_TRUE(harness_.device_manager().close_device(1).has_value());

    nvidia::storage_lender::v1::DeleteCompletionQueueRequest del_req;
    del_req.set_cq_id(1);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::DELETE_COMPLETION_QUEUE), del_req));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::NOT_FOUND));
}

// CREATE_SUBMISSION_QUEUE: valid iova and cq_id but unknown device_id → NOT_FOUND.
TEST_F(ServerQueueIntegrationTest, CreateSqFailsWithUnknownDeviceId)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x7000 }));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    {
        nvidia::storage_lender::v1::OpenDeviceRequest req;
        req.set_pci_address("0000:01:00.0");
        req.set_open_mode(nvidia::storage_lender::v1::SHARED);
        client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD));
    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    client->send_fd_scm_rights(memfd->get());
    nvidia::storage_lender::wire::v1::Response fd_resp;
    client->recv_response(fd_resp);
    nvidia::storage_lender::v1::TransferFdResponse fd_msg;
    fd_msg.ParseFromString(fd_resp.payload());

    nvidia::storage_lender::v1::MapBufferRequest map_req;
    map_req.set_fd_id(fd_msg.fd_id());
    map_req.set_size(4096);
    client->send_request(static_cast<uint32_t>(MethodId::MAP_BUFFER), map_req);
    nvidia::storage_lender::wire::v1::Response map_resp;
    client->recv_response(map_resp);
    nvidia::storage_lender::v1::MapBufferResponse map_msg;
    map_msg.ParseFromString(map_resp.payload());

    {
        nvidia::storage_lender::v1::CreateCompletionQueueRequest req;
        req.set_device_id(1);
        req.set_iova(map_msg.iova());
        req.set_queue_size(64);
        client->send_request(static_cast<uint32_t>(MethodId::CREATE_COMPLETION_QUEUE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    // Valid iova and cq_id, but device_id=999 is unknown.
    nvidia::storage_lender::v1::CreateSubmissionQueueRequest sq_req;
    sq_req.set_device_id(999);
    sq_req.set_cq_id(1);
    sq_req.set_iova(map_msg.iova());
    sq_req.set_queue_size(64);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::CREATE_SUBMISSION_QUEUE), sq_req));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::NOT_FOUND));
}

// CREATE_SUBMISSION_QUEUE: all checks pass but backend create_sq fails → INTERNAL.
TEST_F(ServerQueueIntegrationTest, CreateSqFailsWhenBackendFails)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x7000 }));
    EXPECT_CALL(harness_.backend(), create_sq(CTRLR, _, _, 1, _)).WillOnce(Return(std::unexpected(-1)));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    {
        nvidia::storage_lender::v1::OpenDeviceRequest req;
        req.set_pci_address("0000:01:00.0");
        req.set_open_mode(nvidia::storage_lender::v1::SHARED);
        client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD));
    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    client->send_fd_scm_rights(memfd->get());
    nvidia::storage_lender::wire::v1::Response fd_resp;
    client->recv_response(fd_resp);
    nvidia::storage_lender::v1::TransferFdResponse fd_msg;
    fd_msg.ParseFromString(fd_resp.payload());

    nvidia::storage_lender::v1::MapBufferRequest map_req;
    map_req.set_fd_id(fd_msg.fd_id());
    map_req.set_size(4096);
    client->send_request(static_cast<uint32_t>(MethodId::MAP_BUFFER), map_req);
    nvidia::storage_lender::wire::v1::Response map_resp;
    client->recv_response(map_resp);
    nvidia::storage_lender::v1::MapBufferResponse map_msg;
    map_msg.ParseFromString(map_resp.payload());

    {
        nvidia::storage_lender::v1::CreateCompletionQueueRequest req;
        req.set_device_id(1);
        req.set_iova(map_msg.iova());
        req.set_queue_size(64);
        client->send_request(static_cast<uint32_t>(MethodId::CREATE_COMPLETION_QUEUE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    nvidia::storage_lender::v1::CreateSubmissionQueueRequest sq_req;
    sq_req.set_device_id(1);
    sq_req.set_cq_id(1);
    sq_req.set_iova(map_msg.iova());
    sq_req.set_queue_size(64);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::CREATE_SUBMISSION_QUEUE), sq_req));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::INTERNAL));
}

// DELETE_SUBMISSION_QUEUE: get_ctrlr fails after fault-injecting a missing device → NOT_FOUND.
// The public CloseDevice path cannot produce this state because it rejects live queues.
TEST_F(ServerQueueIntegrationTest, DeleteSqFailsWhenGetCtrlrFails)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x7000 }));
    EXPECT_CALL(harness_.backend(), create_sq(CTRLR, _, _, 1, _))
        .WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x8000 }));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    {
        nvidia::storage_lender::v1::OpenDeviceRequest req;
        req.set_pci_address("0000:01:00.0");
        req.set_open_mode(nvidia::storage_lender::v1::SHARED);
        client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD));
    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    client->send_fd_scm_rights(memfd->get());
    nvidia::storage_lender::wire::v1::Response fd_resp;
    client->recv_response(fd_resp);
    nvidia::storage_lender::v1::TransferFdResponse fd_msg;
    fd_msg.ParseFromString(fd_resp.payload());

    nvidia::storage_lender::v1::MapBufferRequest map_req;
    map_req.set_fd_id(fd_msg.fd_id());
    map_req.set_size(4096);
    client->send_request(static_cast<uint32_t>(MethodId::MAP_BUFFER), map_req);
    nvidia::storage_lender::wire::v1::Response map_resp;
    client->recv_response(map_resp);
    nvidia::storage_lender::v1::MapBufferResponse map_msg;
    map_msg.ParseFromString(map_resp.payload());

    {
        nvidia::storage_lender::v1::CreateCompletionQueueRequest req;
        req.set_device_id(1);
        req.set_iova(map_msg.iova());
        req.set_queue_size(64);
        client->send_request(static_cast<uint32_t>(MethodId::CREATE_COMPLETION_QUEUE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    {
        nvidia::storage_lender::v1::CreateSubmissionQueueRequest req;
        req.set_device_id(1);
        req.set_cq_id(1);
        req.set_iova(map_msg.iova());
        req.set_queue_size(64);
        client->send_request(static_cast<uint32_t>(MethodId::CREATE_SUBMISSION_QUEUE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    ASSERT_TRUE(harness_.device_manager().close_device(1).has_value());

    nvidia::storage_lender::v1::DeleteSubmissionQueueRequest del_req;
    del_req.set_sq_id(1);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::DELETE_SUBMISSION_QUEUE), del_req));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::NOT_FOUND));
}

// DELETE_SUBMISSION_QUEUE: all checks pass but backend delete_sq fails → INTERNAL.
TEST_F(ServerQueueIntegrationTest, DeleteSqFailsWhenBackendFails)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x7000 }));
    EXPECT_CALL(harness_.backend(), create_sq(CTRLR, _, _, 1, _))
        .WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x8000 }));
    EXPECT_CALL(harness_.backend(), delete_sq(CTRLR, 1, _))
        .WillOnce(Return(std::unexpected(-1))) // explicit DELETE_SUBMISSION_QUEUE request
        .WillRepeatedly(Return(std::expected<void, int> { })); // cleanup_session retry on disconnect

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    {
        nvidia::storage_lender::v1::OpenDeviceRequest req;
        req.set_pci_address("0000:01:00.0");
        req.set_open_mode(nvidia::storage_lender::v1::SHARED);
        client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD));
    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    client->send_fd_scm_rights(memfd->get());
    nvidia::storage_lender::wire::v1::Response fd_resp;
    client->recv_response(fd_resp);
    nvidia::storage_lender::v1::TransferFdResponse fd_msg;
    fd_msg.ParseFromString(fd_resp.payload());

    nvidia::storage_lender::v1::MapBufferRequest map_req;
    map_req.set_fd_id(fd_msg.fd_id());
    map_req.set_size(4096);
    client->send_request(static_cast<uint32_t>(MethodId::MAP_BUFFER), map_req);
    nvidia::storage_lender::wire::v1::Response map_resp;
    client->recv_response(map_resp);
    nvidia::storage_lender::v1::MapBufferResponse map_msg;
    map_msg.ParseFromString(map_resp.payload());

    {
        nvidia::storage_lender::v1::CreateCompletionQueueRequest req;
        req.set_device_id(1);
        req.set_iova(map_msg.iova());
        req.set_queue_size(64);
        client->send_request(static_cast<uint32_t>(MethodId::CREATE_COMPLETION_QUEUE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    {
        nvidia::storage_lender::v1::CreateSubmissionQueueRequest req;
        req.set_device_id(1);
        req.set_cq_id(1);
        req.set_iova(map_msg.iova());
        req.set_queue_size(64);
        client->send_request(static_cast<uint32_t>(MethodId::CREATE_SUBMISSION_QUEUE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    nvidia::storage_lender::v1::DeleteSubmissionQueueRequest del_req;
    del_req.set_sq_id(1);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::DELETE_SUBMISSION_QUEUE), del_req));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::INTERNAL));
}

// CREATE_COMPLETION_QUEUE: backend create_cq times out → DEADLINE_EXCEEDED.
TEST_F(ServerQueueIntegrationTest, CreateCqReportsDeadlineOnAdminTimeout)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(std::unexpected(ETIMEDOUT)));

    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    {
        nvidia::storage_lender::v1::OpenDeviceRequest req;
        req.set_pci_address("0000:01:00.0");
        req.set_open_mode(nvidia::storage_lender::v1::SHARED);
        client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD));
    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    client->send_fd_scm_rights(memfd->get());
    nvidia::storage_lender::wire::v1::Response fd_resp;
    client->recv_response(fd_resp);
    nvidia::storage_lender::v1::TransferFdResponse fd_msg;
    fd_msg.ParseFromString(fd_resp.payload());

    nvidia::storage_lender::v1::MapBufferRequest map_req;
    map_req.set_fd_id(fd_msg.fd_id());
    map_req.set_size(4096);
    client->send_request(static_cast<uint32_t>(MethodId::MAP_BUFFER), map_req);
    nvidia::storage_lender::wire::v1::Response map_resp;
    client->recv_response(map_resp);
    nvidia::storage_lender::v1::MapBufferResponse map_msg;
    map_msg.ParseFromString(map_resp.payload());

    nvidia::storage_lender::v1::CreateCompletionQueueRequest cq_req;
    cq_req.set_device_id(1);
    cq_req.set_iova(map_msg.iova());
    cq_req.set_queue_size(64);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::CREATE_COMPLETION_QUEUE), cq_req));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::DEADLINE_EXCEEDED));
}

// DELETE_SUBMISSION_QUEUE: backend delete_sq times out → DEADLINE_EXCEEDED.
TEST_F(ServerQueueIntegrationTest, DeleteSqReportsDeadlineOnAdminTimeout)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x7000 }));
    EXPECT_CALL(harness_.backend(), create_sq(CTRLR, _, _, 1, _))
        .WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x8000 }));
    EXPECT_CALL(harness_.backend(), delete_sq(CTRLR, 1, _))
        .WillOnce(Return(std::unexpected(ETIMEDOUT))) // explicit DELETE_SUBMISSION_QUEUE request
        .WillRepeatedly(Return(std::expected<void, int> { })); // cleanup_session retry on disconnect

    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    {
        nvidia::storage_lender::v1::OpenDeviceRequest req;
        req.set_pci_address("0000:01:00.0");
        req.set_open_mode(nvidia::storage_lender::v1::SHARED);
        client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD));
    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    client->send_fd_scm_rights(memfd->get());
    nvidia::storage_lender::wire::v1::Response fd_resp;
    client->recv_response(fd_resp);
    nvidia::storage_lender::v1::TransferFdResponse fd_msg;
    fd_msg.ParseFromString(fd_resp.payload());

    nvidia::storage_lender::v1::MapBufferRequest map_req;
    map_req.set_fd_id(fd_msg.fd_id());
    map_req.set_size(4096);
    client->send_request(static_cast<uint32_t>(MethodId::MAP_BUFFER), map_req);
    nvidia::storage_lender::wire::v1::Response map_resp;
    client->recv_response(map_resp);
    nvidia::storage_lender::v1::MapBufferResponse map_msg;
    map_msg.ParseFromString(map_resp.payload());

    {
        nvidia::storage_lender::v1::CreateCompletionQueueRequest req;
        req.set_device_id(1);
        req.set_iova(map_msg.iova());
        req.set_queue_size(64);
        client->send_request(static_cast<uint32_t>(MethodId::CREATE_COMPLETION_QUEUE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    {
        nvidia::storage_lender::v1::CreateSubmissionQueueRequest req;
        req.set_device_id(1);
        req.set_cq_id(1);
        req.set_iova(map_msg.iova());
        req.set_queue_size(64);
        client->send_request(static_cast<uint32_t>(MethodId::CREATE_SUBMISSION_QUEUE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    nvidia::storage_lender::v1::DeleteSubmissionQueueRequest del_req;
    del_req.set_sq_id(1);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::DELETE_SUBMISSION_QUEUE), del_req));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::DEADLINE_EXCEEDED));
}
