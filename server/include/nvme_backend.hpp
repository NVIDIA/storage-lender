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

#include "device_policy.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <vector>

using SleepFn = std::function<void(std::chrono::milliseconds)>;

class NvmeBackend {
public:
    virtual ~NvmeBackend() = default;

    virtual std::expected<uint64_t, int> mem_register_dma_buf(int buf_fd, size_t size) = 0;
    virtual std::expected<void, int> mem_unregister_dma_buf(uint64_t iova, size_t size) = 0;

    virtual std::expected<void*, int> nvme_connect(const std::string& pci_address, const NvmeConnectOptions& options)
        = 0;
    virtual std::expected<void, int> nvme_detach(void* ctrlr) = 0;

    struct NsInfo {
        uint32_t ns_id;
        uint32_t block_size;
        uint64_t block_count;
    };

    struct ControllerInfo {
        std::string model;
        std::string pci_resource_path;
        uint32_t max_queue_entries;
        uint32_t page_size;
        uint32_t num_io_queues;
        std::vector<NsInfo> namespaces;
    };

    virtual std::expected<ControllerInfo, int> get_controller_info(void* ctrlr) = 0;

    struct QueueInfo {
        uint32_t qid;
        uint64_t db_offset;
    };

    virtual std::expected<QueueInfo, int> create_cq(
        void* ctrlr, uint64_t iova, uint32_t queue_size, const SleepFn& sleep_fn) = 0;
    virtual std::expected<void, int> delete_cq(void* ctrlr, uint32_t cq_id, const SleepFn& sleep_fn) = 0;
    virtual std::expected<QueueInfo, int> create_sq(
        void* ctrlr, uint64_t iova, uint32_t queue_size, uint32_t cq_id, const SleepFn& sleep_fn) = 0;
    virtual std::expected<void, int> delete_sq(void* ctrlr, uint32_t sq_id, const SleepFn& sleep_fn) = 0;
};
