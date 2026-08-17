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

#pragma once

#include "nvme_backend.hpp"

#include <spdk/nvme.h>

#include <chrono>
#include <cstdint>
#include <functional>

namespace spdk_backend_detail {

using SubmitAdminCommandFn = std::function<int(spdk_nvme_ctrlr*, spdk_nvme_cmd*, spdk_nvme_cmd_cb, void*)>;
using ProcessAdminCompletionsFn = std::function<int32_t(spdk_nvme_ctrlr*)>;
using SteadyClockFn = std::function<std::chrono::steady_clock::time_point()>;

// Harness to allow timeout unit testing (see SpdkAdminCommandTest.LateCompletionAfterTimeoutIsSafe).
struct AdminCommandOps {
    SubmitAdminCommandFn submit;
    ProcessAdminCompletionsFn process_completions;
    SteadyClockFn now;
};

// Uses the default callbacks (the real thing).
int do_admin_cmd_sync(
    spdk_nvme_ctrlr* ctrlr, spdk_nvme_cmd& cmd, const SleepFn& sleep_fn, std::chrono::milliseconds timeout);

// Allow for custom callbacks (eg, mocking).
int do_admin_cmd_sync(spdk_nvme_ctrlr* ctrlr, spdk_nvme_cmd& cmd, const SleepFn& sleep_fn,
    std::chrono::milliseconds timeout, const AdminCommandOps& ops);

}
