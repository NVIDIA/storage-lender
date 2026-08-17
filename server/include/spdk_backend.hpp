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

#include <chrono>
#include <unordered_map>

struct spdk_nvme_cmd;

class SpdkBackend final : public NvmeBackend {
public:
    SpdkBackend();
    ~SpdkBackend() override;

    std::expected<uint64_t, int> mem_register_dma_buf(int buf_fd, size_t size) override;
    std::expected<void, int> mem_unregister_dma_buf(uint64_t iova, size_t size) override;

    std::expected<void*, int> nvme_connect(const std::string& pci_address, const NvmeConnectOptions& options) override;
    std::expected<void, int> nvme_detach(void* ctrlr) override;

    std::expected<ControllerInfo, int> get_controller_info(void* ctrlr) override;

    std::expected<QueueInfo, int> create_cq(
        void* ctrlr, uint64_t iova, uint32_t queue_size, const SleepFn& sleep_fn) override;
    std::expected<void, int> delete_cq(void* ctrlr, uint32_t cq_id, const SleepFn& sleep_fn) override;
    std::expected<QueueInfo, int> create_sq(
        void* ctrlr, uint64_t iova, uint32_t queue_size, uint32_t cq_id, const SleepFn& sleep_fn) override;
    std::expected<void, int> delete_sq(void* ctrlr, uint32_t sq_id, const SleepFn& sleep_fn) override;

private:
    int execute_admin_command(void* ctrlr_handle, spdk_nvme_cmd& cmd, const SleepFn& sleep_fn) const;

    std::unordered_map<void*, std::chrono::milliseconds> admin_command_timeouts_;
};
