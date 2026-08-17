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

#include <sys/mman.h>

#include <cstdint>

using ::testing::_;
using ::testing::Return;

class ServerBufferIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        auto started = harness_.start();
        ASSERT_TRUE(started.has_value()) << started.error().message();
    }

    ServerTestHarness harness_;
};

TEST_F(ServerBufferIntegrationTest, TransferFdRegistersAndMapBuffer)
{
    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD)));

    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    ASSERT_TRUE(client->send_fd_scm_rights(memfd->get()));

    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    ASSERT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::OK));

    nvidia::storage_lender::v1::TransferFdResponse resp_msg;
    ASSERT_TRUE(resp_msg.ParseFromString(resp.payload()));
    EXPECT_GE(resp_msg.fd_id(), 1u);

    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));

    nvidia::storage_lender::v1::MapBufferRequest map_req;
    map_req.set_fd_id(resp_msg.fd_id());
    map_req.set_size(4096);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::MAP_BUFFER), map_req));

    nvidia::storage_lender::wire::v1::Response map_resp;
    ASSERT_TRUE(client->recv_response(map_resp));
    ASSERT_EQ(map_resp.status_code(), static_cast<int32_t>(StatusCode::OK));
}

TEST_F(ServerBufferIntegrationTest, UnmapBufferSucceeds)
{
    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD)));
    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    ASSERT_TRUE(client->send_fd_scm_rights(memfd->get()));
    nvidia::storage_lender::wire::v1::Response fd_resp;
    ASSERT_TRUE(client->recv_response(fd_resp));
    nvidia::storage_lender::v1::TransferFdResponse fd_msg;
    ASSERT_TRUE(fd_msg.ParseFromString(fd_resp.payload()));

    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));
    nvidia::storage_lender::v1::MapBufferRequest map_req;
    map_req.set_fd_id(fd_msg.fd_id());
    map_req.set_size(4096);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::MAP_BUFFER), map_req));
    nvidia::storage_lender::wire::v1::Response map_resp;
    ASSERT_TRUE(client->recv_response(map_resp));
    ASSERT_EQ(map_resp.status_code(), static_cast<int32_t>(StatusCode::OK));
    nvidia::storage_lender::v1::MapBufferResponse map_msg;
    ASSERT_TRUE(map_msg.ParseFromString(map_resp.payload()));

    EXPECT_CALL(harness_.backend(), mem_unregister_dma_buf(_, 4096)).WillOnce(Return(std::expected<void, int> { }));
    nvidia::storage_lender::v1::UnmapBufferRequest unmap_req;
    unmap_req.set_iova(map_msg.iova());
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::UNMAP_BUFFER), unmap_req));
    nvidia::storage_lender::wire::v1::Response unmap_resp;
    ASSERT_TRUE(client->recv_response(unmap_resp));
    ASSERT_EQ(unmap_resp.status_code(), static_cast<int32_t>(StatusCode::OK));
}

TEST_F(ServerBufferIntegrationTest, TransferFdWithoutScmRights)
{
    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD)));
    ASSERT_TRUE(client->send_byte_without_scm());
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::INVALID_ARGUMENT));
}

// TRANSFER_FD: client packs two fds into a single SCM_RIGHTS cmsg.
TEST_F(ServerBufferIntegrationTest, TransferFdWithTwoFdsInOneCmsg)
{
    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD)));
    UniqueFd fd1 { ::memfd_create("test1", MFD_CLOEXEC) };
    UniqueFd fd2 { ::memfd_create("test2", MFD_CLOEXEC) };
    ASSERT_TRUE(fd1);
    ASSERT_TRUE(fd2);
    ASSERT_TRUE(client->send_two_fds_in_one_cmsg(fd1.get(), fd2.get()));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::INVALID_ARGUMENT));
    EXPECT_EQ(resp.error_message(), "expected exactly one file descriptor");
}

// TRANSFER_FD: client overflows the server's ancillary-data buffer with three fds.
TEST_F(ServerBufferIntegrationTest, TransferFdWithTruncatedAncillaryData)
{
    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD)));
    auto fd1 = make_sealed_memfd(4096);
    auto fd2 = make_sealed_memfd(4096);
    auto fd3 = make_sealed_memfd(4096);
    ASSERT_TRUE(fd1.has_value()) << "memfd creation failed (errno=" << fd1.error() << ')';
    ASSERT_TRUE(fd2.has_value()) << "memfd creation failed (errno=" << fd2.error() << ')';
    ASSERT_TRUE(fd3.has_value()) << "memfd creation failed (errno=" << fd3.error() << ')';
    ASSERT_TRUE(client->send_three_fds_in_one_cmsg(fd1->get(), fd2->get(), fd3->get()));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::INVALID_ARGUMENT));
    EXPECT_EQ(resp.error_message(), "truncated ancillary data");
}

// TRANSFER_FD: client sends two separate SCM_RIGHTS cmsgs.
TEST_F(ServerBufferIntegrationTest, TransferFdWithExtraCmsg)
{
    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD)));
    UniqueFd fd1 { ::memfd_create("test1", MFD_CLOEXEC) };
    UniqueFd fd2 { ::memfd_create("test2", MFD_CLOEXEC) };
    ASSERT_TRUE(fd1);
    ASSERT_TRUE(fd2);
    ASSERT_TRUE(client->send_fd_with_extra_cmsg(fd1.get(), fd2.get()));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::INVALID_ARGUMENT));
}

// MAP_BUFFER: mem_register_dma_buf fails → INTERNAL.
TEST_F(ServerBufferIntegrationTest, MapBufferFailsWhenBackendReturnsError)
{
    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    client->send_request(static_cast<uint32_t>(MethodId::TRANSFER_FD));
    auto memfd = make_sealed_memfd(4096);
    ASSERT_TRUE(memfd.has_value()) << "memfd creation failed (errno=" << memfd.error() << ')';
    client->send_fd_scm_rights(memfd->get());
    nvidia::storage_lender::wire::v1::Response fd_resp;
    ASSERT_TRUE(client->recv_response(fd_resp));
    nvidia::storage_lender::v1::TransferFdResponse fd_msg;
    ASSERT_TRUE(fd_msg.ParseFromString(fd_resp.payload()));

    EXPECT_CALL(harness_.backend(), mem_register_dma_buf(_, 4096)).WillOnce(Return(std::unexpected(-1)));

    nvidia::storage_lender::v1::MapBufferRequest map_req;
    map_req.set_fd_id(fd_msg.fd_id());
    map_req.set_size(4096);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::MAP_BUFFER), map_req));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::INTERNAL));
}

// UNMAP_BUFFER: iova not mapped → NOT_FOUND.
TEST_F(ServerBufferIntegrationTest, UnmapBufferFailsWhenNotMapped)
{
    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    nvidia::storage_lender::v1::UnmapBufferRequest req;
    req.set_iova(0xDEADBEEF);
    ASSERT_TRUE(client->send_request(static_cast<uint32_t>(MethodId::UNMAP_BUFFER), req));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::NOT_FOUND));
}
