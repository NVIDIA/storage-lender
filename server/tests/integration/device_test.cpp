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

#include <chrono>
#include <cstdint>
#include <future>

using ::testing::_;
using ::testing::Return;

class ServerDeviceIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        auto started = harness_.start();
        ASSERT_TRUE(started.has_value()) << started.error().message();
    }

    ServerTestHarness harness_;
    void* const CTRLR = reinterpret_cast<void*>(0xCAFE);
};

TEST_F(ServerDeviceIntegrationTest, OpenDeviceReturnsDeviceId)
{
    EXPECT_CALL(harness_.backend(), nvme_connect("0000:01:00.0", _)).WillOnce(Return(CTRLR));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    nvidia::storage_lender::v1::OpenDeviceRequest req;
    req.set_pci_address("0000:01:00.0");
    req.set_open_mode(nvidia::storage_lender::v1::SHARED);

    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), req));

    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    ASSERT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::OK));

    nvidia::storage_lender::v1::OpenDeviceResponse resp_msg;
    ASSERT_TRUE(resp_msg.ParseFromString(resp.payload()));
    EXPECT_EQ(resp_msg.device_id(), 1u);
}

TEST_F(ServerDeviceIntegrationTest, OpenDeviceNotFoundOnNullCtrlr)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(std::unexpected(-1)));

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    nvidia::storage_lender::v1::OpenDeviceRequest req;
    req.set_pci_address("0000:01:00.0");
    req.set_open_mode(nvidia::storage_lender::v1::SHARED);

    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), req));

    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::NOT_FOUND));
}

TEST_F(ServerDeviceIntegrationTest, GetDeviceInfoReturnsModel)
{
    NvmeBackend::ControllerInfo info;
    info.model = "NVMe SSD Pro";
    info.pci_resource_path = "/sys/bus/pci/devices/0000:01:00.0/resource0";
    info.max_queue_entries = 512;
    info.page_size = 4096;
    info.num_io_queues = 8;
    info.namespaces.push_back({ 1, 512, 1000000 });

    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), get_controller_info(CTRLR)).WillOnce(Return(info));

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

    nvidia::storage_lender::v1::DeviceInfoRequest req;
    req.set_device_id(1);

    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::GET_DEVICE_INFO), req));

    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    ASSERT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::OK));

    nvidia::storage_lender::v1::DeviceInfoResponse resp_msg;
    ASSERT_TRUE(resp_msg.ParseFromString(resp.payload()));
    EXPECT_EQ(resp_msg.model(), "NVMe SSD Pro");
    EXPECT_EQ(resp_msg.pci_resource_path(), "/sys/bus/pci/devices/0000:01:00.0/resource0");
    EXPECT_EQ(resp_msg.max_queue_entries(), 512u);
    EXPECT_EQ(resp_msg.page_size(), 4096u);
    EXPECT_EQ(resp_msg.num_io_queues(), 8u);
    ASSERT_EQ(resp_msg.namespaces_size(), 1);
    EXPECT_EQ(resp_msg.namespaces(0).ns_id(), 1u);
    EXPECT_EQ(resp_msg.namespaces(0).block_size(), 512u);
}

TEST_F(ServerDeviceIntegrationTest, CloseDeviceCallsDetach)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), nvme_detach(CTRLR)).Times(1);

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    {
        nvidia::storage_lender::v1::OpenDeviceRequest req;
        req.set_pci_address("0000:01:00.0");
        req.set_open_mode(nvidia::storage_lender::v1::EXCLUSIVE);
        client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), req);
        nvidia::storage_lender::wire::v1::Response resp;
        client->recv_response(resp);
    }

    nvidia::storage_lender::v1::CloseDeviceRequest req;
    req.set_device_id(1);

    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::CLOSE_DEVICE), req));

    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    ASSERT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::OK));
}

TEST_F(ServerDeviceIntegrationTest, DuplicateSharedOpenRequiresMatchingCloses)
{
    EXPECT_CALL(harness_.backend(), nvme_connect("0000:01:00.0", _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), nvme_detach(CTRLR)).Times(1);

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    nvidia::storage_lender::v1::OpenDeviceRequest open_req;
    open_req.set_pci_address("0000:01:00.0");
    open_req.set_open_mode(nvidia::storage_lender::v1::SHARED);

    std::vector<uint32_t> device_ids;
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), open_req));

        nvidia::storage_lender::wire::v1::Response open_resp;
        ASSERT_TRUE(client->recv_response(open_resp));
        ASSERT_EQ(open_resp.status_code(), static_cast<int32_t>(StatusCode::OK));

        nvidia::storage_lender::v1::OpenDeviceResponse open_resp_msg;
        ASSERT_TRUE(open_resp_msg.ParseFromString(open_resp.payload()));
        device_ids.push_back(open_resp_msg.device_id());
    }
    ASSERT_EQ(device_ids.size(), 2u);
    EXPECT_NE(device_ids[0], device_ids[1]);

    for (uint32_t device_id : device_ids) {
        nvidia::storage_lender::v1::CloseDeviceRequest close_req;
        close_req.set_device_id(device_id);
        ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::CLOSE_DEVICE), close_req));

        nvidia::storage_lender::wire::v1::Response close_resp;
        ASSERT_TRUE(client->recv_response(close_resp));
        EXPECT_EQ(close_resp.status_code(), static_cast<int32_t>(StatusCode::OK));
    }
}

TEST_F(ServerDeviceIntegrationTest, DisconnectReleasesAllDuplicateSharedOpens)
{
    EXPECT_CALL(harness_.backend(), nvme_connect("0000:01:00.0", _)).WillOnce(Return(CTRLR));
    std::promise<void> device_detached;
    EXPECT_CALL(harness_.backend(), nvme_detach(CTRLR)).WillOnce([&device_detached](void*) -> std::expected<void, int> {
        device_detached.set_value();
        return { };
    });

    {
        auto client = harness_.connect_protocol();
        ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
        nvidia::storage_lender::v1::OpenDeviceRequest open_req;
        open_req.set_pci_address("0000:01:00.0");
        open_req.set_open_mode(nvidia::storage_lender::v1::SHARED);

        for (int i = 0; i < 2; ++i) {
            ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), open_req));

            nvidia::storage_lender::wire::v1::Response open_resp;
            ASSERT_TRUE(client->recv_response(open_resp));
            ASSERT_EQ(open_resp.status_code(), static_cast<int32_t>(StatusCode::OK));
        }
    }

    ASSERT_EQ(device_detached.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
}

TEST_F(ServerDeviceIntegrationTest, ExclusiveConflictReturnsPermissionDenied)
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

    nvidia::storage_lender::v1::OpenDeviceRequest req;
    req.set_pci_address("0000:01:00.0");
    req.set_open_mode(nvidia::storage_lender::v1::EXCLUSIVE);

    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), req));

    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::PERMISSION_DENIED));
}

TEST_F(ServerDeviceIntegrationTest, CloseDeviceFailsWhenQueueStillExists)
{
    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x7000 }));
    EXPECT_CALL(harness_.backend(), delete_cq(CTRLR, 1, _)).WillOnce(Return(std::expected<void, int> { }));
    EXPECT_CALL(harness_.backend(), nvme_detach(CTRLR)).Times(1);

    auto client = harness_.connect_protocol();

    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    nvidia::storage_lender::v1::OpenDeviceRequest open_req;
    open_req.set_pci_address("0000:01:00.0");
    open_req.set_open_mode(nvidia::storage_lender::v1::SHARED);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::OPEN_DEVICE), open_req));
    nvidia::storage_lender::wire::v1::Response open_resp;
    ASSERT_TRUE(client->recv_response(open_resp));
    ASSERT_EQ(open_resp.status_code(), static_cast<int32_t>(StatusCode::OK));

    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD)));
    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    ASSERT_TRUE(client->send_fd_scm_rights(memfd->get()));
    nvidia::storage_lender::wire::v1::Response fd_resp;
    ASSERT_TRUE(client->recv_response(fd_resp));
    ASSERT_EQ(fd_resp.status_code(), static_cast<int32_t>(StatusCode::OK));
    nvidia::storage_lender::v1::TransferFdResponse fd_msg;
    ASSERT_TRUE(fd_msg.ParseFromString(fd_resp.payload()));

    nvidia::storage_lender::v1::MapBufferRequest map_req;
    map_req.set_fd_id(fd_msg.fd_id());
    map_req.set_size(4096);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::MAP_BUFFER), map_req));
    nvidia::storage_lender::wire::v1::Response map_resp;
    ASSERT_TRUE(client->recv_response(map_resp));
    ASSERT_EQ(map_resp.status_code(), static_cast<int32_t>(StatusCode::OK));
    nvidia::storage_lender::v1::MapBufferResponse map_msg;
    ASSERT_TRUE(map_msg.ParseFromString(map_resp.payload()));

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

    nvidia::storage_lender::v1::CloseDeviceRequest close_req;
    close_req.set_device_id(1);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::CLOSE_DEVICE), close_req));
    nvidia::storage_lender::wire::v1::Response close_resp;
    ASSERT_TRUE(client->recv_response(close_resp));
    EXPECT_EQ(close_resp.status_code(), static_cast<int32_t>(StatusCode::FAILED_PRECONDITION));

    nvidia::storage_lender::v1::DeleteCompletionQueueRequest delete_req;
    delete_req.set_cq_id(cq_msg.cq_id());
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::DELETE_COMPLETION_QUEUE), delete_req));
    nvidia::storage_lender::wire::v1::Response delete_resp;
    ASSERT_TRUE(client->recv_response(delete_resp));
    ASSERT_EQ(delete_resp.status_code(), static_cast<int32_t>(StatusCode::OK));

    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::CLOSE_DEVICE), close_req));
    ASSERT_TRUE(client->recv_response(close_resp));
    EXPECT_EQ(close_resp.status_code(), static_cast<int32_t>(StatusCode::OK));
}

TEST_F(ServerDeviceIntegrationTest, CloseDeviceFailsWhenNotFound)
{
    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    nvidia::storage_lender::v1::CloseDeviceRequest req;
    req.set_device_id(999);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::CLOSE_DEVICE), req));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::NOT_FOUND));
}

// GET_DEVICE_INFO: unknown device_id → NOT_FOUND.
TEST_F(ServerDeviceIntegrationTest, GetDeviceInfoFailsWhenNotFound)
{
    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    nvidia::storage_lender::v1::DeviceInfoRequest req;
    req.set_device_id(999);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::GET_DEVICE_INFO), req));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::NOT_FOUND));
}
