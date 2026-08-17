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

#include "storage_lender/client.hpp"

#include "device_manager.hpp"
#include "mock_nvme_backend.hpp"
#include "server.hpp"
#include "server_test_harness.hpp"
#include "test_dma_buffer.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

using storage_lender::ClientError;
using storage_lender::DeviceInfo;
using storage_lender::QueueInfo;
using storage_lender::StorageLenderClient;

class ClientTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        auto started = harness_.start();
        ASSERT_TRUE(started.has_value()) << started.error().message();
    }

    ServerTestHarness harness_;
    void* const CTRLR = reinterpret_cast<void*>(0xCAFE);
};

class StorageLenderClientQuotaTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        QuotaPolicy policy;
        policy.mode = QuotaMode::ENFORCED;
        policy.default_principal.device_handles = 0;
        ServerConfig config {
            .api_socket = { },
            .device_policy = test_device_policy(),
            .quota_policy = std::move(policy),
            .principal_resolver = PrincipalResolver { { }, { } },
        };
        auto started = harness_.start(std::move(config), default_test_peer_credentials(), { }, "client-quota");
        ASSERT_TRUE(started.has_value()) << started.error().message();
    }

    ServerTestHarness harness_;
};

TEST_F(StorageLenderClientQuotaTest, RpcDenialMapsToResourceExhausted)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).Times(0);

    StorageLenderClient client;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client), ClientError::OK);
    uint32_t device_id;
    EXPECT_EQ(client.open_device("0000:01:00.0", storage_lender::OpenDeviceMode::SHARED, device_id),
        ClientError::RESOURCE_EXHAUSTED);
}

TEST_F(ClientTest, ConnectSucceeds)
{
    StorageLenderClient client;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client), ClientError::OK);
}

TEST_F(ClientTest, ConnectFailsOnBadPath)
{
    StorageLenderClient client;
    const auto missing_path = harness_.socket_path() + ".missing";
    EXPECT_EQ(StorageLenderClient::connect(missing_path, client), ClientError::IO_ERROR);
}

TEST_F(ClientTest, TransferFdReturnsId)
{
    StorageLenderClient client;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client), ClientError::OK);

    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';

    uint32_t fd_id;
    auto err = client.transfer_fd(memfd->get(), fd_id);

    ASSERT_EQ(err, ClientError::OK);
    EXPECT_GE(fd_id, 1u);
}

TEST_F(ClientTest, TransferFdRejectsUnsealedFd)
{
    StorageLenderClient client;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client), ClientError::OK);

    auto memfd = make_unsealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';

    uint32_t fd_id;
    auto err = client.transfer_fd(memfd->get(), fd_id);

    EXPECT_EQ(err, ClientError::INVALID_ARGUMENT);
}

TEST_F(ClientTest, MapAndUnmapBuffer)
{
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), mem_unregister_dma_buf(_, 4096)).WillOnce(Return(std::expected<void, int> { }));

    StorageLenderClient client;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client), ClientError::OK);

    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    uint32_t fd_id;
    ASSERT_EQ(client.transfer_fd(memfd->get(), fd_id), ClientError::OK);

    uint64_t iova;
    ASSERT_EQ(client.map_buffer(fd_id, 4096, iova), ClientError::OK);
    ASSERT_EQ(client.unmap_buffer(iova), ClientError::OK);
}

TEST_F(ClientTest, MapBufferRejectsSizeLargerThanTransferredFd)
{
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, _)).Times(0);

    StorageLenderClient client;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client), ClientError::OK);

    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    uint32_t fd_id;
    ASSERT_EQ(client.transfer_fd(memfd->get(), fd_id), ClientError::OK);

    uint64_t iova;
    EXPECT_EQ(client.map_buffer(fd_id, 8192, iova), ClientError::INVALID_ARGUMENT);
}

TEST_F(ClientTest, MapBufferRejectsNonPageMultipleSize)
{
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, _)).Times(0);

    StorageLenderClient client;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client), ClientError::OK);

    auto memfd = make_sealed_memfd(system_page_size() * 2);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    uint32_t fd_id;
    ASSERT_EQ(client.transfer_fd(memfd->get(), fd_id), ClientError::OK);

    uint64_t iova;
    EXPECT_EQ(client.map_buffer(fd_id, system_page_size() + 1, iova), ClientError::INVALID_ARGUMENT);
}

TEST_F(ClientTest, OpenAndCloseDevice)
{
    EXPECT_CALL(harness_.backend(), nvme_connect("0000:01:00.0", _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), nvme_detach(CTRLR)).WillOnce(Return(std::expected<void, int> { }));

    StorageLenderClient client;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client), ClientError::OK);

    uint32_t device_id;
    ASSERT_EQ(client.open_device("0000:01:00.0", storage_lender::OpenDeviceMode::SHARED, device_id), ClientError::OK);
    EXPECT_GE(device_id, 1u);
    ASSERT_EQ(client.close_device(device_id), ClientError::OK);
}

TEST_F(ClientTest, GetDeviceInfo)
{
    NvmeBackend::ControllerInfo info;
    info.model = "TestDrive";
    info.pci_resource_path = "/sys/bus/pci/devices/0000:01:00.0/resource0";
    info.max_queue_entries = 1024;
    info.page_size = 4096;
    info.num_io_queues = 4;
    info.namespaces.push_back({ 1, 512, 500000 });

    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), get_controller_info(CTRLR)).WillOnce(Return(info));

    StorageLenderClient client;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client), ClientError::OK);
    uint32_t device_id;
    ASSERT_EQ(client.open_device("0000:01:00.0", storage_lender::OpenDeviceMode::SHARED, device_id), ClientError::OK);

    DeviceInfo result;
    ASSERT_EQ(client.get_device_info(device_id, result), ClientError::OK);
    EXPECT_EQ(result.model, "TestDrive");
    EXPECT_EQ(result.pci_resource_path, "/sys/bus/pci/devices/0000:01:00.0/resource0");
    EXPECT_EQ(result.max_queue_entries, 1024u);
    EXPECT_EQ(result.page_size, 4096u);
    EXPECT_EQ(result.num_io_queues, 4u);
    ASSERT_EQ(result.namespaces.size(), 1u);
    EXPECT_EQ(result.namespaces[0].ns_id, 1u);
    EXPECT_EQ(result.namespaces[0].block_size, 512u);
    EXPECT_EQ(result.namespaces[0].block_count, 500000u);
}

TEST_F(ClientTest, CreateAndDeleteCq)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x7000 }));
    EXPECT_CALL(harness_.backend(), delete_cq(CTRLR, 1, _)).WillOnce(Return(std::expected<void, int> { }));

    StorageLenderClient client;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client), ClientError::OK);
    uint32_t device_id;
    ASSERT_EQ(client.open_device("0000:01:00.0", storage_lender::OpenDeviceMode::SHARED, device_id), ClientError::OK);

    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    uint32_t fd_id;
    ASSERT_EQ(client.transfer_fd(memfd->get(), fd_id), ClientError::OK);
    uint64_t iova;
    ASSERT_EQ(client.map_buffer(fd_id, 4096, iova), ClientError::OK);

    QueueInfo cq;
    ASSERT_EQ(client.create_cq(device_id, iova, 64, cq), ClientError::OK);
    EXPECT_GE(cq.qid, 1u);
    EXPECT_EQ(cq.db_offset, 0x7000u);
    ASSERT_EQ(client.delete_cq(cq.qid), ClientError::OK);
}

TEST_F(ClientTest, CreateAndDeleteSq)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x7000 }));
    EXPECT_CALL(harness_.backend(), create_sq(CTRLR, _, _, 1, _))
        .WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x8000 }));
    EXPECT_CALL(harness_.backend(), delete_sq(CTRLR, 1, _)).WillOnce(Return(std::expected<void, int> { }));
    EXPECT_CALL(harness_.backend(), delete_cq(CTRLR, 1, _)).WillOnce(Return(std::expected<void, int> { }));

    StorageLenderClient client;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client), ClientError::OK);
    uint32_t device_id;
    ASSERT_EQ(client.open_device("0000:01:00.0", storage_lender::OpenDeviceMode::SHARED, device_id), ClientError::OK);

    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    uint32_t fd_id;
    ASSERT_EQ(client.transfer_fd(memfd->get(), fd_id), ClientError::OK);
    uint64_t iova;
    ASSERT_EQ(client.map_buffer(fd_id, 4096, iova), ClientError::OK);

    QueueInfo cq;
    ASSERT_EQ(client.create_cq(device_id, iova, 64, cq), ClientError::OK);

    QueueInfo sq;
    ASSERT_EQ(client.create_sq(device_id, cq.qid, iova, 64, sq), ClientError::OK);
    EXPECT_GE(sq.qid, 1u);
    EXPECT_EQ(sq.db_offset, 0x8000u);

    ASSERT_EQ(client.delete_sq(sq.qid), ClientError::OK);
    ASSERT_EQ(client.delete_cq(cq.qid), ClientError::OK);
}

TEST_F(ClientTest, ErrorMappedCorrectly)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(std::unexpected(-1)));

    StorageLenderClient client;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client), ClientError::OK);

    uint32_t device_id;
    EXPECT_EQ(
        client.open_device("0000:ff:00.0", storage_lender::OpenDeviceMode::SHARED, device_id), ClientError::NOT_FOUND);
}

TEST_F(ClientTest, MoveAssignment)
{
    EXPECT_CALL(harness_.backend(), nvme_connect("0000:01:00.0", _)).WillOnce(Return(CTRLR));

    StorageLenderClient client1;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client1), ClientError::OK);
    StorageLenderClient client2;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client2), ClientError::OK);
    client2 = std::move(client1);

    uint32_t device_id;
    ASSERT_EQ(client2.open_device("0000:01:00.0", storage_lender::OpenDeviceMode::SHARED, device_id), ClientError::OK);
}

TEST_F(ClientTest, ClientReturnsInvalidArgumentFromServer)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));

    StorageLenderClient client;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client), ClientError::OK);
    uint32_t device_id;
    ASSERT_EQ(client.open_device("0000:01:00.0", storage_lender::OpenDeviceMode::SHARED, device_id), ClientError::OK);

    QueueInfo cq;
    EXPECT_EQ(client.create_cq(device_id, 0xDEADBEEF, 64, cq), ClientError::INVALID_ARGUMENT);
}

TEST_F(ClientTest, CreateCqRejectsInvalidQueueSizes)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(_, _, _, _)).Times(0);

    StorageLenderClient client;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client), ClientError::OK);
    uint32_t device_id;
    ASSERT_EQ(client.open_device("0000:01:00.0", storage_lender::OpenDeviceMode::SHARED, device_id), ClientError::OK);

    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    uint32_t fd_id;
    ASSERT_EQ(client.transfer_fd(memfd->get(), fd_id), ClientError::OK);
    uint64_t iova;
    ASSERT_EQ(client.map_buffer(fd_id, 4096, iova), ClientError::OK);

    QueueInfo cq;
    EXPECT_EQ(client.create_cq(device_id, iova, 0, cq), ClientError::INVALID_ARGUMENT);
    EXPECT_EQ(client.create_cq(device_id, iova, 1, cq), ClientError::INVALID_ARGUMENT);
    EXPECT_EQ(client.create_cq(device_id, iova, 257, cq), ClientError::INVALID_ARGUMENT);
}

TEST_F(ClientTest, CreateSqRejectsInvalidQueueSizes)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x7000 }));
    EXPECT_CALL(harness_.backend(), create_sq(_, _, _, _, _)).Times(0);

    StorageLenderClient client;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client), ClientError::OK);
    uint32_t device_id;
    ASSERT_EQ(client.open_device("0000:01:00.0", storage_lender::OpenDeviceMode::SHARED, device_id), ClientError::OK);

    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    uint32_t fd_id;
    ASSERT_EQ(client.transfer_fd(memfd->get(), fd_id), ClientError::OK);
    uint64_t iova;
    ASSERT_EQ(client.map_buffer(fd_id, 4096, iova), ClientError::OK);

    QueueInfo cq;
    ASSERT_EQ(client.create_cq(device_id, iova, 64, cq), ClientError::OK);

    QueueInfo sq;
    EXPECT_EQ(client.create_sq(device_id, cq.qid, iova, 0, sq), ClientError::INVALID_ARGUMENT);
    EXPECT_EQ(client.create_sq(device_id, cq.qid, iova, 1, sq), ClientError::INVALID_ARGUMENT);
    EXPECT_EQ(client.create_sq(device_id, cq.qid, iova, 65, sq), ClientError::INVALID_ARGUMENT);
}

TEST_F(ClientTest, ClientReturnsPermissionDeniedFromServer)
{
    EXPECT_CALL(harness_.backend(), nvme_connect("0000:01:00.0", _)).WillOnce(Return(CTRLR));

    StorageLenderClient client1;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client1), ClientError::OK);
    uint32_t device_id1;
    ASSERT_EQ(client1.open_device("0000:01:00.0", storage_lender::OpenDeviceMode::SHARED, device_id1), ClientError::OK);

    StorageLenderClient client2;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client2), ClientError::OK);
    uint32_t device_id2;
    EXPECT_EQ(client2.open_device("0000:01:00.0", storage_lender::OpenDeviceMode::EXCLUSIVE, device_id2),
        ClientError::PERMISSION_DENIED);
}

TEST_F(ClientTest, ClientReturnsFailedPreconditionFromServer)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));
    EXPECT_CALL(harness_.backend(), create_sq(CTRLR, _, _, 1, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0 }));

    StorageLenderClient client;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client), ClientError::OK);
    uint32_t device_id;
    ASSERT_EQ(client.open_device("0000:01:00.0", storage_lender::OpenDeviceMode::SHARED, device_id), ClientError::OK);

    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    uint32_t fd_id;
    ASSERT_EQ(client.transfer_fd(memfd->get(), fd_id), ClientError::OK);
    uint64_t iova;
    ASSERT_EQ(client.map_buffer(fd_id, 4096, iova), ClientError::OK);

    QueueInfo cq;
    ASSERT_EQ(client.create_cq(device_id, iova, 64, cq), ClientError::OK);
    QueueInfo sq;
    ASSERT_EQ(client.create_sq(device_id, cq.qid, iova, 64, sq), ClientError::OK);

    EXPECT_EQ(client.delete_cq(cq.qid), ClientError::FAILED_PRECONDITION);
}

TEST_F(ClientTest, DoorbellOffsetPropagatesEndToEnd)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 3, 0x100C }));
    EXPECT_CALL(harness_.backend(), create_sq(CTRLR, _, _, 3, _))
        .WillOnce(Return(NvmeBackend::QueueInfo { 3, 0x1018 }));
    EXPECT_CALL(harness_.backend(), delete_sq(CTRLR, 3, _)).WillOnce(Return(std::expected<void, int> { }));
    EXPECT_CALL(harness_.backend(), delete_cq(CTRLR, 3, _)).WillOnce(Return(std::expected<void, int> { }));

    StorageLenderClient client;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client), ClientError::OK);
    uint32_t device_id;
    ASSERT_EQ(client.open_device("0000:01:00.0", storage_lender::OpenDeviceMode::SHARED, device_id), ClientError::OK);

    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    uint32_t fd_id;
    ASSERT_EQ(client.transfer_fd(memfd->get(), fd_id), ClientError::OK);
    uint64_t iova;
    ASSERT_EQ(client.map_buffer(fd_id, 4096, iova), ClientError::OK);

    QueueInfo cq;
    ASSERT_EQ(client.create_cq(device_id, iova, 64, cq), ClientError::OK);
    EXPECT_EQ(cq.db_offset, 0x100Cu);

    QueueInfo sq;
    ASSERT_EQ(client.create_sq(device_id, cq.qid, iova, 64, sq), ClientError::OK);
    EXPECT_EQ(sq.db_offset, 0x1018u);

    client.delete_sq(sq.qid);
    client.delete_cq(cq.qid);
}

TEST_F(ClientTest, CreateCqReportsDeadlineExceededOnAdminTimeout)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(std::unexpected(ETIMEDOUT)));

    StorageLenderClient client;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client), ClientError::OK);

    uint32_t device_id = 0;
    ASSERT_EQ(client.open_device("0000:01:00.0", storage_lender::OpenDeviceMode::SHARED, device_id), ClientError::OK);

    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    uint32_t fd_id = 0;
    ASSERT_EQ(client.transfer_fd(memfd->get(), fd_id), ClientError::OK);

    uint64_t iova = 0;
    ASSERT_EQ(client.map_buffer(fd_id, 4096, iova), ClientError::OK);

    QueueInfo cq;
    EXPECT_EQ(client.create_cq(device_id, iova, 64, cq), ClientError::DEADLINE_EXCEEDED);
}

TEST_F(ClientTest, RpcAfterServerStopReturnsIoErrorWithoutSigpipe)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));

    StorageLenderClient client;
    ASSERT_EQ(StorageLenderClient::connect(harness_.socket_path(), client), ClientError::OK);

    uint32_t device_id = 0;
    ASSERT_EQ(client.open_device("0000:01:00.0", storage_lender::OpenDeviceMode::SHARED, device_id), ClientError::OK);

    // Tear the daemon down mid-session. This test intentionally installs NO SIGPIPE
    // handler: the next write must surface a clean IO_ERROR, not kill the process.
    harness_.stop();

    uint32_t device_id_after = 0;
    EXPECT_EQ(client.open_device("0000:01:00.0", storage_lender::OpenDeviceMode::SHARED, device_id_after),
        ClientError::IO_ERROR);
}
