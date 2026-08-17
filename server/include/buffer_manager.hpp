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

struct ReceivedFdEntry {
    int fd;
    uint64_t size;
    QuotaLease quota;
};

struct MappedBufferEntry {
    uint64_t size;
    int fd;
    QuotaLease quota;
};

class BufferManager {
public:
    explicit BufferManager(NvmeBackend& backend);
    ~BufferManager();

    static std::expected<uint64_t, LenderError> validate_fd(int fd);
    std::expected<uint32_t, LenderError> register_fd(int fd, uint64_t validated_size, QuotaLease quota);

    std::expected<void, LenderError> validate_mapping(uint32_t fd_id, uint64_t size) const;
    std::expected<uint64_t, LenderError> map_buffer(
        uint32_t fd_id, uint64_t size, uint32_t alignment, QuotaLease quota);

    std::expected<void, LenderError> unmap_buffer(uint64_t iova);

    bool has_iova(uint64_t iova) const;
    std::expected<uint64_t, LenderError> buf_size(uint64_t iova) const;

    void cleanup_all();

private:
    NvmeBackend& backend_;

    uint32_t next_fd_id_ { 1 };

    std::unordered_map<uint32_t, ReceivedFdEntry> received_fds_;
    std::unordered_map<uint64_t, MappedBufferEntry> mapped_buffers_;
};
