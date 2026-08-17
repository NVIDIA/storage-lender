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

#include "lender_error.hpp"
#include "nvme_backend.hpp"
#include "quota_manager.hpp"

#include <cstdint>
#include <expected>
#include <unordered_map>
#include <vector>

struct CqEntry {
    uint32_t device_id;
    uint64_t iova;
    uint32_t size;
    uint64_t cq_db_offset;
    QuotaLease quota;
};

struct SqEntry {
    uint32_t device_id;
    uint32_t cq_id;
    uint64_t iova;
    uint64_t sq_db_offset;
    QuotaLease quota;
};

class QueueManager {
public:
    explicit QueueManager(NvmeBackend& backend);
    ~QueueManager();

    std::expected<NvmeBackend::QueueInfo, LenderError> create_cq(
        void* ctrlr, uint32_t device_id, uint64_t iova, uint32_t queue_size, const SleepFn& sleep_fn, QuotaLease quota);

    std::expected<void, LenderError> delete_cq(void* ctrlr, uint32_t cq_id, const SleepFn& sleep_fn);
    void orphan_cq(uint32_t cq_id);

    std::expected<NvmeBackend::QueueInfo, LenderError> create_sq(void* ctrlr, uint32_t device_id, uint32_t cq_id,
        uint64_t iova, uint32_t queue_size, const SleepFn& sleep_fn, QuotaLease quota);

    std::expected<void, LenderError> delete_sq(void* ctrlr, uint32_t sq_id, const SleepFn& sleep_fn);
    void orphan_sq(uint32_t sq_id);

    bool has_cq(uint32_t cq_id) const;
    bool has_sqs_for_cq(uint32_t cq_id) const;
    bool has_queues_on_device(uint32_t device_id) const;
    uint32_t device_of_cq(uint32_t cq_id) const;
    uint32_t device_of_sq(uint32_t sq_id) const;
    std::vector<uint32_t> all_cq_ids() const;
    std::vector<uint32_t> all_sq_ids() const;

private:
    void orphan_all();

    NvmeBackend& backend_;

    std::unordered_map<uint32_t, CqEntry> cqs_;
    std::unordered_map<uint32_t, SqEntry> sqs_;
};
