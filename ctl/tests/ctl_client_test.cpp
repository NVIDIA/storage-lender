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

#include "ctl_client.hpp"

#include "storage_lender/wire/framing.hpp"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <utility>

namespace {

using nvidia::storage_lender::wire::v1::Request;
using nvidia::storage_lender::wire::v1::Response;
using nvidia::storage_lender::wire::v1::StatusCode;

bool read_exact(int socket_fd, void* buffer, std::size_t size)
{
    std::size_t total = 0;
    while (total < size) {
        const auto count = ::read(socket_fd, static_cast<char*>(buffer) + total, size - total);
        if (count <= 0) {
            return false;
        }
        total += static_cast<std::size_t>(count);
    }
    return true;
}

bool write_exact(int socket_fd, const void* buffer, std::size_t size)
{
    std::size_t total = 0;
    while (total < size) {
        const auto count = ::send(socket_fd, static_cast<const char*>(buffer) + total, size - total, MSG_NOSIGNAL);
        if (count <= 0) {
            return false;
        }
        total += static_cast<std::size_t>(count);
    }
    return true;
}

bool read_frame(int socket_fd, std::string& payload)
{
    storage_lender::wire::FrameHeader header { };
    if (!read_exact(socket_fd, header.data(), header.size())) {
        return false;
    }
    uint32_t payload_size = 0;
    if (!storage_lender::wire::decode_frame_header(header, payload_size)) {
        return false;
    }
    payload.resize(payload_size);
    return payload_size == 0 || read_exact(socket_fd, payload.data(), payload.size());
}

bool write_frame(int socket_fd, std::string_view payload)
{
    storage_lender::wire::FrameHeader header { };
    if (!storage_lender::wire::encode_frame_header(payload.size(), header)) {
        return false;
    }
    return write_exact(socket_fd, header.data(), header.size())
        && write_exact(socket_fd, payload.data(), payload.size());
}

class CtlClientTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        std::error_code error;
        const auto temp_directory = std::filesystem::temp_directory_path(error);
        ASSERT_FALSE(error) << error.message();
        static std::atomic<uint64_t> next_id { };
        directory_ = temp_directory
            / ("storage-lender-ctl-" + std::to_string(::getpid()) + '-' + std::to_string(next_id.fetch_add(1)));
        ASSERT_TRUE(std::filesystem::create_directory(directory_, error)) << error.message();
        socket_path_ = (directory_ / "ctl.sock").string();

        listener_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        ASSERT_GE(listener_fd_, 0) << ::strerror(errno);
        sockaddr_un address { };
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_path_.data(), socket_path_.size());
        ASSERT_EQ(::bind(listener_fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0)
            << ::strerror(errno);
        ASSERT_EQ(::listen(listener_fd_, 1), 0) << ::strerror(errno);
    }

    void TearDown() override
    {
        if (listener_fd_ >= 0) {
            ::close(listener_fd_);
            listener_fd_ = -1;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        std::error_code error;
        std::filesystem::remove(socket_path_, error);
        EXPECT_FALSE(error) << error.message();
        std::filesystem::remove(directory_, error);
        EXPECT_FALSE(error) << error.message();
    }

    void serve_once(std::function<void(int)> handler)
    {
        worker_ = std::thread([this, handler = std::move(handler)] {
            const auto client_fd = ::accept4(listener_fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (client_fd >= 0) {
                handler(client_fd);
                ::close(client_fd);
            }
        });
    }

    std::filesystem::path directory_;
    std::string socket_path_;
    int listener_fd_ { -1 };
    std::thread worker_;
};

TEST_F(CtlClientTest, SendsEmptyQuotaRequestAndParsesResponse)
{
    Request request;
    bool request_received = false;
    serve_once([&](int client_fd) {
        std::string payload;
        request_received = read_frame(client_fd, payload) && request.ParseFromString(payload);

        nvidia::storage_lender::ctl::v1::GetQuotaStateResponse quota_state;
        quota_state.set_generation(42);
        Response response;
        response.set_status_code(StatusCode::OK);
        response.set_payload(quota_state.SerializeAsString());
        write_frame(client_fd, response.SerializeAsString());
    });

    auto response = storage_lender::ctl::get_quota_state(socket_path_);

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->generation(), 42U);
    if (worker_.joinable()) {
        worker_.join();
    }
    ASSERT_TRUE(request_received);
    EXPECT_EQ(request.method(), static_cast<uint32_t>(nvidia::storage_lender::ctl::v1::GET_QUOTA_STATE));
    EXPECT_TRUE(request.payload().empty());
}

TEST_F(CtlClientTest, SendsEmptyLatencyRequestAndParsesResponse)
{
    Request request;
    bool request_received = false;
    serve_once([&](int client_fd) {
        std::string payload;
        request_received = read_frame(client_fd, payload) && request.ParseFromString(payload);

        nvidia::storage_lender::ctl::v1::GetCommandLatencyResponse command_latency;
        command_latency.set_capacity_per_command(256);
        Response response;
        response.set_status_code(StatusCode::OK);
        response.set_payload(command_latency.SerializeAsString());
        write_frame(client_fd, response.SerializeAsString());
    });

    auto response = storage_lender::ctl::get_command_latency(socket_path_);

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->capacity_per_command(), 256U);
    if (worker_.joinable()) {
        worker_.join();
    }
    ASSERT_TRUE(request_received);
    EXPECT_EQ(request.method(), static_cast<uint32_t>(nvidia::storage_lender::ctl::v1::GET_COMMAND_LATENCY));
    EXPECT_TRUE(request.payload().empty());
}

TEST_F(CtlClientTest, LatencyCallRejectsUnparseableResponseBody)
{
    serve_once([](int client_fd) {
        std::string payload;
        if (!read_frame(client_fd, payload)) {
            return;
        }
        Response response;
        response.set_status_code(StatusCode::OK);
        response.set_payload(std::string(1, '\x80'));
        write_frame(client_fd, response.SerializeAsString());
    });

    auto response = storage_lender::ctl::get_command_latency(socket_path_);

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().category, storage_lender::ctl::CtlClientError::PROTOCOL);
    EXPECT_EQ(response.error().message, "failed to parse command latency response");
}

TEST_F(CtlClientTest, ReturnsServerStatusForNonOkResponse)
{
    serve_once([](int client_fd) {
        std::string payload;
        if (!read_frame(client_fd, payload)) {
            return;
        }
        Response response;
        response.set_status_code(StatusCode::PERMISSION_DENIED);
        response.set_error_message("root required");
        write_frame(client_fd, response.SerializeAsString());
    });

    auto response = storage_lender::ctl::get_quota_state(socket_path_);

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().category, storage_lender::ctl::CtlClientError::SERVER_STATUS);
    EXPECT_EQ(response.error().server_status, StatusCode::PERMISSION_DENIED);
    EXPECT_EQ(response.error().message, "root required");
}

TEST_F(CtlClientTest, ReturnsProtocolForUnparseableResponseBody)
{
    serve_once([](int client_fd) {
        std::string payload;
        if (!read_frame(client_fd, payload)) {
            return;
        }
        Response response;
        response.set_status_code(StatusCode::OK);
        response.set_payload(std::string(1, '\x80'));
        write_frame(client_fd, response.SerializeAsString());
    });

    auto response = storage_lender::ctl::get_quota_state(socket_path_);

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().category, storage_lender::ctl::CtlClientError::PROTOCOL);
}

} // anonymous namespace
