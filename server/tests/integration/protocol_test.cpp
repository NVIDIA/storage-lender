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
#include "storage_lender/wire/framing.hpp"

#include "client.pb.h"
#include "wire.pb.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <ranges>
#include <string>

class ServerProtocolIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        auto started = harness_.start();
        ASSERT_TRUE(started.has_value()) << started.error().message();
    }

    ServerTestHarness harness_;
};

TEST(ClientProtocolWireCompatibilityTest, SplitEnvelopePreservesClientPayloadBytes)
{
    nvidia::storage_lender::v1::OpenDeviceRequest payload;
    payload.set_pci_address("0000:01:00.0");
    payload.set_open_mode(nvidia::storage_lender::v1::EXCLUSIVE);

    nvidia::storage_lender::wire::v1::Request request;
    request.set_method(static_cast<uint32_t>(MethodId::OPEN_DEVICE));
    request.set_payload(payload.SerializeAsString());

    const auto serialized = request.SerializeAsString();
    ASSERT_EQ(serialized.size(), payload.ByteSizeLong() + 4U);
    EXPECT_EQ(static_cast<unsigned char>(serialized[0]), 0x08U);
    EXPECT_EQ(static_cast<unsigned char>(serialized[1]), 0x04U);
    EXPECT_EQ(static_cast<unsigned char>(serialized[2]), 0x12U);
    EXPECT_EQ(static_cast<unsigned char>(serialized[3]), payload.ByteSizeLong());
    EXPECT_EQ(serialized.substr(4), payload.SerializeAsString());
}

TEST_F(ServerProtocolIntegrationTest, UnknownMethodReturnsInvalidArgument)
{
    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    ASSERT_TRUE(client->send_request(9999u));

    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::INVALID_ARGUMENT));
}

TEST_F(ServerProtocolIntegrationTest, AllHandlerGarbagePayloads)
{
    const std::string garbage(1, '\x80');
    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';

    std::ranges::for_each(
        std::initializer_list<uint32_t> {
            static_cast<uint32_t>(MethodId::MAP_BUFFER),
            static_cast<uint32_t>(MethodId::UNMAP_BUFFER),
            static_cast<uint32_t>(MethodId::OPEN_DEVICE),
            static_cast<uint32_t>(MethodId::CLOSE_DEVICE),
            static_cast<uint32_t>(MethodId::GET_DEVICE_INFO),
            static_cast<uint32_t>(MethodId::CREATE_COMPLETION_QUEUE),
            static_cast<uint32_t>(MethodId::DELETE_COMPLETION_QUEUE),
            static_cast<uint32_t>(MethodId::CREATE_SUBMISSION_QUEUE),
            static_cast<uint32_t>(MethodId::DELETE_SUBMISSION_QUEUE),
        },
        [&](uint32_t method) {
            ASSERT_TRUE(client->send_request_raw_payload(method, garbage)) << "method=" << method;
            nvidia::storage_lender::wire::v1::Response resp;
            ASSERT_TRUE(client->recv_response(resp)) << "method=" << method;
            EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::INVALID_ARGUMENT)) << "method=" << method;
        });
}

// Sending a frame whose content is invalid protobuf causes client_loop to break.
TEST_F(ServerProtocolIntegrationTest, RequestParseErrorDropsConnection)
{
    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    ASSERT_TRUE(client->send_raw_request_frame(std::string(1, '\x80')));
    nvidia::storage_lender::wire::v1::Response resp;
    EXPECT_FALSE(client->recv_response(resp));
}

// The frame-size boundary remains usable, and processing it does not prevent the
// connection from serving a subsequent small request.
TEST_F(ServerProtocolIntegrationTest, MaximumSizedFrameKeepsConnectionUsable)
{
    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    nvidia::storage_lender::wire::v1::Request req;
    req.set_method(UINT32_MAX);
    req.set_payload(std::string(storage_lender::wire::MAX_FRAME_BYTES, '\0'));

    auto frame = req.SerializeAsString();
    ASSERT_GT(frame.size(), storage_lender::wire::MAX_FRAME_BYTES);
    req.mutable_payload()->resize(req.payload().size() - (frame.size() - storage_lender::wire::MAX_FRAME_BYTES));
    frame = req.SerializeAsString();
    ASSERT_EQ(frame.size(), storage_lender::wire::MAX_FRAME_BYTES);

    ASSERT_TRUE(client->send_raw_request_frame(frame));
    nvidia::storage_lender::wire::v1::Response resp;
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::INVALID_ARGUMENT));

    ASSERT_TRUE(client->send_request(UINT32_MAX));
    ASSERT_TRUE(client->recv_response(resp));
    EXPECT_EQ(resp.status_code(), static_cast<int32_t>(StatusCode::INVALID_ARGUMENT));
}

// A length header exceeding MAX_FRAME_BYTES causes read_frame to drop the connection.
TEST_F(ServerProtocolIntegrationTest, OversizedFrameDropsConnection)
{
    auto client = harness_.connect_protocol();
    ASSERT_TRUE(client.has_value()) << "protocol connect failed (errno=" << client.error() << ')';
    ASSERT_TRUE(client->send_oversized_header());
    nvidia::storage_lender::wire::v1::Response resp;
    EXPECT_FALSE(client->recv_response(resp));
}
