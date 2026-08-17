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

#include <atomic>
#include <cerrno>
#include <memory>

namespace spdk_backend_detail {

namespace {

    struct AdminCmdCtx {
        std::atomic<bool> done = false;
        uint16_t status = 0;
    };

    struct AdminCompletion {
        std::shared_ptr<AdminCmdCtx> context;
    };

    void on_admin_complete(void* arg, const struct spdk_nvme_cpl* cpl)
    {
        auto completion = std::unique_ptr<AdminCompletion> { static_cast<AdminCompletion*>(arg) };
        auto& ctx = *completion->context;
        ctx.status = spdk_nvme_cpl_is_error(cpl) ? cpl->status_raw : 0;
        ctx.done.store(true, std::memory_order_release);
    }

    const AdminCommandOps DEFAULT_ADMIN_COMMAND_OPS {
        .submit =
            [](spdk_nvme_ctrlr* ctrlr, spdk_nvme_cmd* cmd, spdk_nvme_cmd_cb callback, void* callback_arg) {
                return spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, cmd, nullptr, 0, callback, callback_arg);
            },
        .process_completions = [](spdk_nvme_ctrlr* ctrlr) { return spdk_nvme_ctrlr_process_admin_completions(ctrlr); },
        .now = [] { return std::chrono::steady_clock::now(); },
    };

} // Anonymous namespace.

int do_admin_cmd_sync(
    spdk_nvme_ctrlr* ctrlr, spdk_nvme_cmd& cmd, const SleepFn& sleep_fn, std::chrono::milliseconds timeout)
{
    return do_admin_cmd_sync(ctrlr, cmd, sleep_fn, timeout, DEFAULT_ADMIN_COMMAND_OPS);
}

int do_admin_cmd_sync(spdk_nvme_ctrlr* ctrlr, spdk_nvme_cmd& cmd, const SleepFn& sleep_fn,
    std::chrono::milliseconds timeout, const AdminCommandOps& ops)
{
    auto ctx = std::make_shared<AdminCmdCtx>();
    auto completion = std::make_unique<AdminCompletion>(ctx);
    if (const auto rc = ops.submit(ctrlr, &cmd, on_admin_complete, completion.get()); rc != 0) {
        return rc;
    }
    // SPDK now owns this completion through the callback.
    completion.release();

    const auto deadline = ops.now() + timeout;
    while (!ctx->done.load(std::memory_order_acquire)) {
        if (ops.now() > deadline) {
            // The callback-owned reference keeps ctx alive until SPDK completes the command.
            return ETIMEDOUT;
        }
        ops.process_completions(ctrlr);
        if (!ctx->done.load(std::memory_order_acquire)) {
            sleep_fn(std::chrono::milliseconds(1));
        }
    }

    return ctx->status;
}

}
