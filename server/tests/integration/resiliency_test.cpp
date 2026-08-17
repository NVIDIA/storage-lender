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
#include <future>
#include <string>

#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

using ::testing::_;
using ::testing::Return;

class ServerResiliencyIntegrationTest : public ::testing::Test {
protected:
    ServerTestHarness harness_;
    void* const CTRLR = reinterpret_cast<void*>(0xCAFE);
};

// A client that dies mid-frame (length prefix sent, body truncated) must be torn
// down cleanly: no hang, and all resources released.
TEST_F(ServerResiliencyIntegrationTest, PartialFrameThenDisconnectCleansUp)
{
    auto started = harness_.start();
    ASSERT_TRUE(started.has_value()) << started.error().message();

    EXPECT_CALL(harness_.backend(), nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    EXPECT_CALL(harness_.backend(), create_cq(CTRLR, _, _, _)).WillOnce(Return(NvmeBackend::QueueInfo { 1, 0x7000 }));
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
        }

        // Declare a 128-byte body but send only 10 bytes, then drop the connection.
        ASSERT_TRUE(client->send_partial_frame(128, std::string(10, '\0')));
        // client goes out of scope here → socket closes mid-frame.
    }

    ASSERT_EQ(cleanup_done.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready)
        << "server did not clean up after a partial-frame disconnect";
}

// An abruptly killed client process must have all its server-side resources
// (session, device, buffer, CQ, SQ) released via the socket-EOF cleanup path.
TEST_F(ServerResiliencyIntegrationTest, AbruptClientKillReleasesResources)
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

    std::promise<void> cleanup_done;
    EXPECT_CALL(harness_.backend(), nvme_detach(CTRLR)).WillOnce([&cleanup_done](void*) -> std::expected<void, int> {
        cleanup_done.set_value();
        return { };
    });

    const std::string socket_path = harness_.socket_path();

    int ready_pipe[2] = { -1, -1 };
    ASSERT_EQ(::pipe(ready_pipe), 0);

    const pid_t child = ::fork();
    ASSERT_GE(child, 0) << "fork failed (errno=" << errno << ')';

    if (child == 0) {
        // Child: acquire resources against the parent's server, signal readiness, then block.
        ::close(ready_pipe[0]);
        auto client = ProtocolTestClient::connect(socket_path);
        if (!client) {
            _exit(10);
        }
        if (!client->open_shared_device("0000:01:00.0")) {
            _exit(11);
        }
        auto memfd = make_sealed_memfd(4096);
        if (!memfd) {
            _exit(12);
        }
        auto fd_id = client->transfer_fd(memfd->get());
        if (!fd_id) {
            _exit(13);
        }
        auto iova = client->map_buffer(*fd_id, 4096);
        if (!iova) {
            _exit(14);
        }
        // The child deliberately relies on fork-safe allocation: the parent's io thread is
        // parked in epoll_wait at fork time, holding no malloc/protobuf locks.
        nvidia::storage_lender::v1::CreateCompletionQueueRequest cq_req;
        cq_req.set_device_id(1);
        cq_req.set_iova(*iova);
        cq_req.set_queue_size(64);
        auto cq_resp = client->call(MethodId::CREATE_COMPLETION_QUEUE, cq_req);
        if (!cq_resp || cq_resp->status_code() != static_cast<int32_t>(StatusCode::OK)) {
            _exit(15);
        }
        nvidia::storage_lender::v1::CreateCompletionQueueResponse cq_msg;
        cq_msg.ParseFromString(cq_resp->payload());
        nvidia::storage_lender::v1::CreateSubmissionQueueRequest sq_req;
        sq_req.set_device_id(1);
        sq_req.set_cq_id(cq_msg.cq_id());
        sq_req.set_iova(*iova);
        sq_req.set_queue_size(64);
        auto sq_resp = client->call(MethodId::CREATE_SUBMISSION_QUEUE, sq_req);
        if (!sq_resp || sq_resp->status_code() != static_cast<int32_t>(StatusCode::OK)) {
            _exit(16);
        }
        const char ready = 1;
        if (::write(ready_pipe[1], &ready, 1) != 1) {
            _exit(17);
        }
        for (;;) {
            ::pause();
        }
    }

    // Parent.
    ::close(ready_pipe[1]);
    char ready = 0;
    ASSERT_EQ(::read(ready_pipe[0], &ready, 1), 1) << "child did not acquire resources (check its _exit code)";
    ::close(ready_pipe[0]);

    const auto before = harness_.usage_snapshot();
    EXPECT_EQ(before.global_usage.active.device_handles, 1u);
    EXPECT_EQ(before.global_usage.active.completion_queues, 1u);
    EXPECT_EQ(before.global_usage.active.submission_queues, 1u);

    ASSERT_EQ(::kill(child, SIGKILL), 0);
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGKILL);

    ASSERT_EQ(cleanup_done.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready)
        << "server did not release resources after abrupt client kill";
    EXPECT_TRUE(
        harness_.wait_for_usage([](const UsageSnapshot& u) { return u.global_usage.active == QuotaAmounts { }; }));
}
