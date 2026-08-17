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

#include "client.pb.h"
#include "ctl.pb.h"
#include "protocol.hpp"
#include "server_test_harness.hpp"

#include <google/protobuf/util/json_util.h>
#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <expected>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct CommandResult {
    int exit_status;
    std::string stdout_output;
    std::string stderr_output;
};

std::expected<std::string, int> read_all(int file_descriptor)
{
    std::string output;
    std::array<char, 4096> buffer { };
    for (;;) {
        const auto count = ::read(file_descriptor, buffer.data(), buffer.size());
        if (count == 0) {
            return output;
        }
        if (count < 0) {
            return std::unexpected(errno);
        }
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
}

std::expected<CommandResult, int> run_ctl(std::initializer_list<std::string_view> arguments)
{
    std::vector<std::string> argument_storage;
    argument_storage.reserve(arguments.size());
    for (const auto argument : arguments) {
        argument_storage.emplace_back(argument);
    }
    std::vector<char*> argv;
    argv.reserve(argument_storage.size() + 2);
    argv.push_back(const_cast<char*>(STORAGE_LENDER_CTL_PATH));
    for (auto& argument : argument_storage) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    std::array<int, 2> stdout_pipe { -1, -1 };
    std::array<int, 2> stderr_pipe { -1, -1 };
    if (::pipe2(stdout_pipe.data(), O_CLOEXEC) != 0) {
        return std::unexpected(errno);
    }
    if (::pipe2(stderr_pipe.data(), O_CLOEXEC) != 0) {
        const auto system_error = errno;
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        return std::unexpected(system_error);
    }

    const auto child_pid = ::fork();
    if (child_pid < 0) {
        const auto system_error = errno;
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        return std::unexpected(system_error);
    }
    if (child_pid == 0) {
        ::close(stdout_pipe[0]);
        ::close(stderr_pipe[0]);
        if (::dup2(stdout_pipe[1], STDOUT_FILENO) < 0 || ::dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[1]);
        ::execv(STORAGE_LENDER_CTL_PATH, argv.data());
        _exit(127);
    }

    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);
    int wait_status = 0;
    while (::waitpid(child_pid, &wait_status, 0) < 0) {
        if (errno != EINTR) {
            const auto system_error = errno;
            ::close(stdout_pipe[0]);
            ::close(stderr_pipe[0]);
            return std::unexpected(system_error);
        }
    }
    auto stdout_output = read_all(stdout_pipe[0]);
    const auto stdout_error = stdout_output ? 0 : stdout_output.error();
    ::close(stdout_pipe[0]);
    auto stderr_output = read_all(stderr_pipe[0]);
    const auto stderr_error = stderr_output ? 0 : stderr_output.error();
    ::close(stderr_pipe[0]);
    if (!stdout_output) {
        return std::unexpected(stdout_error);
    }
    if (!stderr_output) {
        return std::unexpected(stderr_error);
    }
    return CommandResult {
        .exit_status = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 127,
        .stdout_output = std::move(*stdout_output),
        .stderr_output = std::move(*stderr_output),
    };
}

class CtlIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        auto started = harness_.start();
        ASSERT_TRUE(started.has_value()) << started.error().message();
    }

    void record_failed_get_device_info()
    {
        auto client = harness_.connect_protocol();
        ASSERT_TRUE(client.has_value()) << "API connect failed (errno=" << client.error() << ')';
        nvidia::storage_lender::v1::DeviceInfoRequest request;
        request.set_device_id(999);
        auto response = client->call(MethodId::GET_DEVICE_INFO, request);
        ASSERT_TRUE(response.has_value()) << "API request failed (errno=" << response.error() << ')';
        ASSERT_EQ(response->status_code(), nvidia::storage_lender::wire::v1::NOT_FOUND);
    }

    ServerTestHarness harness_;
};

TEST_F(CtlIntegrationTest, RendersHumanQuotaState)
{
    auto result = run_ctl({ "--socket", harness_.ctl_socket_path(), "quota" });

    ASSERT_TRUE(result.has_value()) << "ctl invocation failed (errno=" << result.error() << ')';
    EXPECT_EQ(result->exit_status, 0);
    EXPECT_NE(result->stdout_output.find("global"), std::string::npos);
    EXPECT_TRUE(result->stderr_output.empty());
}

TEST_F(CtlIntegrationTest, RendersProtobufJson)
{
    auto result = run_ctl({ "-s", harness_.ctl_socket_path(), "--format", "json", "quota" });

    ASSERT_TRUE(result.has_value()) << "ctl invocation failed (errno=" << result.error() << ')';
    EXPECT_EQ(result->exit_status, 0);
    EXPECT_TRUE(result->stderr_output.empty());
    nvidia::storage_lender::ctl::v1::GetQuotaStateResponse response;
    const auto status = google::protobuf::util::JsonStringToMessage(result->stdout_output, &response);
    ASSERT_TRUE(status.ok()) << status.ToString();
    EXPECT_EQ(response.generation(), 1U);
}

TEST_F(CtlIntegrationTest, RendersHumanCommandLatency)
{
    record_failed_get_device_info();

    auto result = run_ctl({ "--socket", harness_.ctl_socket_path(), "latency" });

    ASSERT_TRUE(result.has_value()) << "ctl invocation failed (errno=" << result.error() << ')';
    EXPECT_EQ(result->exit_status, 0);
    EXPECT_TRUE(result->stderr_output.empty());
    EXPECT_NE(result->stdout_output.find("command latency"), std::string::npos);
    EXPECT_NE(result->stdout_output.find("get_device_info"), std::string::npos);
}

TEST_F(CtlIntegrationTest, RendersCommandLatencyProtobufJson)
{
    record_failed_get_device_info();

    auto result = run_ctl({ "-s", harness_.ctl_socket_path(), "--format", "json", "latency" });

    ASSERT_TRUE(result.has_value()) << "ctl invocation failed (errno=" << result.error() << ')';
    EXPECT_EQ(result->exit_status, 0);
    EXPECT_TRUE(result->stderr_output.empty());
    nvidia::storage_lender::ctl::v1::GetCommandLatencyResponse response;
    const auto status = google::protobuf::util::JsonStringToMessage(result->stdout_output, &response);
    ASSERT_TRUE(status.ok()) << status.ToString();

    const nvidia::storage_lender::ctl::v1::CommandLatencySummary* get_device_info = nullptr;
    for (const auto& command : response.commands()) {
        if (command.command() == "get_device_info") {
            get_device_info = &command;
            break;
        }
    }
    ASSERT_NE(get_device_info, nullptr);
    EXPECT_EQ(get_device_info->samples(), 1U);
    EXPECT_EQ(get_device_info->errors(), 1U);
}

TEST_F(CtlIntegrationTest, RejectsInvalidFormatWithoutConnecting)
{
    auto result = run_ctl({ "--format", "invalid", "quota" });

    ASSERT_TRUE(result.has_value()) << "ctl invocation failed (errno=" << result.error() << ')';
    EXPECT_EQ(result->exit_status, 2);
    EXPECT_TRUE(result->stdout_output.empty());
    EXPECT_NE(result->stderr_output.find("unknown format: invalid"), std::string::npos);
}

TEST_F(CtlIntegrationTest, ReportsMissingSocket)
{
    auto result = run_ctl({ "--socket", "/tmp/storage-lender-missing-ctl.sock", "quota" });

    ASSERT_TRUE(result.has_value()) << "ctl invocation failed (errno=" << result.error() << ')';
    EXPECT_EQ(result->exit_status, 1);
    EXPECT_TRUE(result->stdout_output.empty());
    EXPECT_NE(result->stderr_output.find("failed to connect to ctl socket"), std::string::npos);
}

} // anonymous namespace
