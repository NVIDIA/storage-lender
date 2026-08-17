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

#include "detail/posix_raii.hpp"

#include "client.pb.h"
#include "wire.pb.h"

#include <boost/asio/post.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <sys/stat.h>

#include <chrono>
#include <filesystem>
#include <future>

using ::testing::_;
using ::testing::Return;

namespace {
using storage_lender::server_detail::ScopedUmask;

} // anonymous namespace

class ServerSessionIntegrationTest : public ::testing::Test {
protected:
    ServerTestHarness harness_;
    void* const CTRLR = reinterpret_cast<void*>(0xCAFE);
};

TEST_F(ServerSessionIntegrationTest, SocketPermissionsAreOwnerAndGroupOnly)
{
    {
        ScopedUmask umask { 0 };
        auto started = harness_.start();
        ASSERT_TRUE(started.has_value()) << started.error().message();
    }

    struct stat socket_stat { };
    EXPECT_EQ(::stat(harness_.socket_path().c_str(), &socket_stat), 0);
    EXPECT_EQ(socket_stat.st_mode & ACCESSPERMS, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

    const auto socket_parent = std::filesystem::path { harness_.socket_path() }.parent_path();
    struct stat parent_stat { };
    EXPECT_EQ(::stat(socket_parent.c_str(), &parent_stat), 0);
    EXPECT_EQ(parent_stat.st_mode & ACCESSPERMS, S_IRWXU);
}

TEST_F(ServerSessionIntegrationTest, CtlSocketPermissionsAreOwnerOnly)
{
    {
        ScopedUmask umask { 0 };
        auto started = harness_.start();
        ASSERT_TRUE(started.has_value()) << started.error().message();
    }

    struct stat socket_stat { };
    EXPECT_EQ(::stat(harness_.ctl_socket_path().c_str(), &socket_stat), 0);
    EXPECT_EQ(socket_stat.st_mode & ACCESSPERMS, S_IRUSR | S_IWUSR);
}

TEST_F(ServerSessionIntegrationTest, SocketPathIsRemovedOnServerDestruction)
{
    auto started = harness_.start();
    ASSERT_TRUE(started.has_value()) << started.error().message();
    const auto path = harness_.socket_path();
    ASSERT_TRUE(std::filesystem::exists(path));

    harness_.stop();

    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(ServerSessionIntegrationTest, CtlSocketPathIsRemovedOnServerDestruction)
{
    auto started = harness_.start();
    ASSERT_TRUE(started.has_value()) << started.error().message();
    const auto path = harness_.ctl_socket_path();
    ASSERT_TRUE(std::filesystem::exists(path));

    harness_.stop();

    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(ServerSessionIntegrationTest, SessionCleanupDeletesQueuesOnDisconnect)
{
    auto started = harness_.start();
    ASSERT_TRUE(started.has_value()) << started.error().message();

    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x7000 }));
    EXPECT_CALL(harness_.backend(), create_sq(CTRLR, _, _, 1, _))
        .WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x8000 }));

    EXPECT_CALL(harness_.backend(), delete_sq(CTRLR, 1, _)).WillOnce(Return(std::expected<void, int> { }));
    EXPECT_CALL(harness_.backend(), delete_cq(CTRLR, 1, _)).WillOnce(Return(std::expected<void, int> { }));

    std::promise<void> device_closed;
    EXPECT_CALL(harness_.backend(), nvme_detach(CTRLR)).WillOnce([&device_closed](void*) -> std::expected<void, int> {
        device_closed.set_value();
        return { };
    });

    {
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
        }

        // client disconnects here
    }

    auto f = device_closed.get_future();
    ASSERT_EQ(f.wait_for(std::chrono::seconds(2)), std::future_status::ready);
}

TEST_F(ServerSessionIntegrationTest, ShutdownCancelsActiveSessions)
{
    auto started = harness_.start();
    ASSERT_TRUE(started.has_value()) << started.error().message();

    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x7000 }));
    EXPECT_CALL(harness_.backend(), create_sq(CTRLR, _, _, 1, _))
        .WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x8000 }));
    EXPECT_CALL(harness_.backend(), delete_sq(CTRLR, 1, _)).WillOnce(Return(std::expected<void, int> { }));
    EXPECT_CALL(harness_.backend(), delete_cq(CTRLR, 1, _)).WillOnce(Return(std::expected<void, int> { }));

    std::promise<void> device_closed;
    EXPECT_CALL(harness_.backend(), nvme_detach(CTRLR)).WillOnce([&device_closed](void*) -> std::expected<void, int> {
        device_closed.set_value();
        return { };
    });

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
    }

    // Trigger shutdown while the client is still connected; cleanup_session should run
    boost::asio::post(harness_.io_context(), [this] { harness_.server().shutdown(); });

    auto f = device_closed.get_future();
    ASSERT_EQ(f.wait_for(std::chrono::seconds(2)), std::future_status::ready);
}

// Cleanup must continue with CQ and device release after an SQ deletion failure.
TEST_F(ServerSessionIntegrationTest, CleanupContinuesAfterDeleteSqFailure)
{
    auto started = harness_.start();
    ASSERT_TRUE(started.has_value()) << started.error().message();

    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x7000 }));
    EXPECT_CALL(harness_.backend(), create_sq(CTRLR, _, _, 1, _))
        .WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x8000 }));
    EXPECT_CALL(harness_.backend(), delete_sq(CTRLR, 1, _)).WillOnce(Return(std::unexpected(-1)));
    EXPECT_CALL(harness_.backend(), delete_cq(CTRLR, 1, _)).WillOnce(Return(std::expected<void, int> { }));

    std::promise<void> cleanup_done;
    EXPECT_CALL(harness_.backend(), nvme_detach(CTRLR)).WillOnce([&cleanup_done](void*) -> std::expected<void, int> {
        cleanup_done.set_value();
        return { };
    });

    {
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

            nvidia::storage_lender::v1::CreateSubmissionQueueRequest sq_req;
            sq_req.set_device_id(1);
            sq_req.set_cq_id(1);
            sq_req.set_iova(map_msg.iova());
            sq_req.set_queue_size(64);
            client->send_request(static_cast<uint32_t>(MethodId::CREATE_SUBMISSION_QUEUE), sq_req);
            nvidia::storage_lender::wire::v1::Response sq_resp;
            client->recv_response(sq_resp);
        }

        // client disconnects here → cleanup_session runs
    }

    ASSERT_EQ(cleanup_done.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
}
