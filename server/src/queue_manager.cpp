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

#include "queue_manager.hpp"

#include <algorithm>
#include <cerrno>
#include <ranges>
#include <utility>

namespace {

LenderError backend_error_to_lender(int rc)
{
    return rc == ETIMEDOUT ? LenderError::DEADLINE_EXCEEDED : LenderError::INTERNAL;
}

} // namespace

QueueManager::QueueManager(NvmeBackend& backend)
    : backend_(backend)
{
}

QueueManager::~QueueManager() { orphan_all(); }

std::expected<NvmeBackend::QueueInfo, LenderError> QueueManager::create_cq(
    void* ctrlr, uint32_t device_id, uint64_t iova, uint32_t queue_size, const SleepFn& sleep_fn, QuotaLease quota)
{
    auto r = backend_.create_cq(ctrlr, iova, queue_size, sleep_fn);
    if (!r) {
        return std::unexpected(backend_error_to_lender(r.error()));
    }
    if (cqs_.contains(r->qid)) {
        auto rollback = backend_.delete_cq(ctrlr, r->qid, sleep_fn);
        if (!rollback) {
            quota.mark_orphaned();
        }
        return std::unexpected(LenderError::INTERNAL);
    }
    cqs_.emplace(r->qid, CqEntry { device_id, iova, queue_size, r->db_offset, std::move(quota) });
    return *r;
}

std::expected<void, LenderError> QueueManager::delete_cq(void* ctrlr, uint32_t cq_id, const SleepFn& sleep_fn)
{
    if (!cqs_.contains(cq_id)) {
        return std::unexpected(LenderError::NOT_FOUND);
    }
    auto r = backend_.delete_cq(ctrlr, cq_id, sleep_fn);
    if (!r) {
        return std::unexpected(backend_error_to_lender(r.error()));
    }
    cqs_.erase(cq_id);
    return { };
}

void QueueManager::orphan_cq(uint32_t cq_id)
{
    auto it = cqs_.find(cq_id);
    if (it == cqs_.end()) {
        return;
    }
    it->second.quota.mark_orphaned();
    cqs_.erase(it);
}

std::expected<NvmeBackend::QueueInfo, LenderError> QueueManager::create_sq(void* ctrlr, uint32_t device_id,
    uint32_t cq_id, uint64_t iova, uint32_t queue_size, const SleepFn& sleep_fn, QuotaLease quota)
{
    auto r = backend_.create_sq(ctrlr, iova, queue_size, cq_id, sleep_fn);
    if (!r) {
        return std::unexpected(backend_error_to_lender(r.error()));
    }
    if (sqs_.contains(r->qid)) {
        auto rollback = backend_.delete_sq(ctrlr, r->qid, sleep_fn);
        if (!rollback) {
            quota.mark_orphaned();
        }
        return std::unexpected(LenderError::INTERNAL);
    }
    sqs_.emplace(r->qid, SqEntry { device_id, cq_id, iova, r->db_offset, std::move(quota) });
    return *r;
}

std::expected<void, LenderError> QueueManager::delete_sq(void* ctrlr, uint32_t sq_id, const SleepFn& sleep_fn)
{
    if (!sqs_.contains(sq_id)) {
        return std::unexpected(LenderError::NOT_FOUND);
    }
    auto r = backend_.delete_sq(ctrlr, sq_id, sleep_fn);
    if (!r) {
        return std::unexpected(backend_error_to_lender(r.error()));
    }
    sqs_.erase(sq_id);
    return { };
}

void QueueManager::orphan_sq(uint32_t sq_id)
{
    auto it = sqs_.find(sq_id);
    if (it == sqs_.end()) {
        return;
    }
    it->second.quota.mark_orphaned();
    sqs_.erase(it);
}

void QueueManager::orphan_all()
{
    for (auto& entry : sqs_ | std::views::values) {
        entry.quota.mark_orphaned();
    }
    sqs_.clear();

    for (auto& entry : cqs_ | std::views::values) {
        entry.quota.mark_orphaned();
    }
    cqs_.clear();
}

std::vector<uint32_t> QueueManager::all_cq_ids() const
{
    auto keys = cqs_ | std::views::keys;
    return { keys.begin(), keys.end() };
}

std::vector<uint32_t> QueueManager::all_sq_ids() const
{
    auto keys = sqs_ | std::views::keys;
    return { keys.begin(), keys.end() };
}

bool QueueManager::has_cq(uint32_t cq_id) const { return cqs_.contains(cq_id); }

bool QueueManager::has_sqs_for_cq(uint32_t cq_id) const
{
    return std::ranges::any_of(sqs_, [cq_id](const auto& entry) { return entry.second.cq_id == cq_id; });
}

bool QueueManager::has_queues_on_device(uint32_t device_id) const
{
    auto belongs_to_device = [device_id](const auto& entry) { return entry.second.device_id == device_id; };
    return std::ranges::any_of(cqs_, belongs_to_device) || std::ranges::any_of(sqs_, belongs_to_device);
}

uint32_t QueueManager::device_of_cq(uint32_t cq_id) const
{
    auto it = cqs_.find(cq_id);
    return (it != cqs_.end()) ? it->second.device_id : 0;
}

uint32_t QueueManager::device_of_sq(uint32_t sq_id) const
{
    auto it = sqs_.find(sq_id);
    return (it != sqs_.end()) ? it->second.device_id : 0;
}
