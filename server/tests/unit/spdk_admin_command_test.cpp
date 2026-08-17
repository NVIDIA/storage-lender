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

#include "spdk_admin_command.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>

namespace {

int polls_before_timeout(std::chrono::milliseconds timeout)
{
    spdk_nvme_cmd_cb saved_callback = nullptr;
    void* saved_callback_arg = nullptr;
    auto now = std::chrono::steady_clock::time_point { };
    int poll_count = 0;
    spdk_backend_detail::AdminCommandOps ops {
        .submit =
            [&](spdk_nvme_ctrlr*, spdk_nvme_cmd*, spdk_nvme_cmd_cb callback, void* callback_arg) {
                saved_callback = callback;
                saved_callback_arg = callback_arg;
                return 0;
            },
        .process_completions =
            [&](spdk_nvme_ctrlr*) {
                ++poll_count;
                return 0;
            },
        .now =
            [&] {
                const auto current = now;
                now += std::chrono::milliseconds { 1 };
                return current;
            },
    };
    spdk_nvme_cmd cmd { };
    const SleepFn sleep_fn = [](std::chrono::milliseconds) { };

    EXPECT_EQ(spdk_backend_detail::do_admin_cmd_sync(nullptr, cmd, sleep_fn, timeout, ops), ETIMEDOUT);
    EXPECT_NE(saved_callback, nullptr);
    if (saved_callback != nullptr) {
        const spdk_nvme_cpl completion { };
        saved_callback(saved_callback_arg, &completion);
    }
    return poll_count;
}

TEST(SpdkAdminCommandTest, CallerSelectedTimeoutControlsPollingDeadline)
{
    EXPECT_EQ(polls_before_timeout(std::chrono::milliseconds { 1 }), 1);
    EXPECT_EQ(polls_before_timeout(std::chrono::milliseconds { 3 }), 3);
}

// Running this test under ASAN also confirms AdminCmdCtx and AdminCompletion are correctly released.
TEST(SpdkAdminCommandTest, LateCompletionAfterTimeoutIsSafe)
{
    spdk_nvme_cmd_cb saved_callback = nullptr;
    void* saved_callback_arg = nullptr;
    auto now = std::chrono::steady_clock::time_point { };
    int poll_count = 0;

    spdk_backend_detail::AdminCommandOps ops {
        .submit =
            [&](spdk_nvme_ctrlr*, spdk_nvme_cmd*, spdk_nvme_cmd_cb callback, void* callback_arg) {
                saved_callback = callback;
                saved_callback_arg = callback_arg;
                return 0;
            },
        .process_completions =
            [&](spdk_nvme_ctrlr*) {
                ++poll_count;
                return 0;
            },
        .now =
            [&] {
                const auto current = now;
                now += std::chrono::milliseconds(1);
                return current;
            },
    };
    spdk_nvme_cmd cmd { };
    const SleepFn sleep_fn = [](std::chrono::milliseconds) { };

    EXPECT_EQ(spdk_backend_detail::do_admin_cmd_sync(nullptr, cmd, sleep_fn, std::chrono::milliseconds { 1 }, ops),
        ETIMEDOUT);
    EXPECT_EQ(poll_count, 1);
    ASSERT_NE(saved_callback, nullptr);

    const spdk_nvme_cpl completion { };
    saved_callback(saved_callback_arg, &completion);
}

}
