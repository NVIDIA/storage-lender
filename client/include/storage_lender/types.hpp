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

#include <cstdint>
#include <string>
#include <vector>

namespace storage_lender {

enum class OpenDeviceMode : int { SHARED = 0, EXCLUSIVE = 1 };

struct NamespaceInfo {
    uint32_t ns_id;
    uint32_t block_size;
    uint64_t block_count;
};

struct DeviceInfo {
    std::string model;
    std::string pci_resource_path;
    uint32_t max_queue_entries;
    uint32_t page_size;
    uint32_t num_io_queues;
    std::vector<NamespaceInfo> namespaces;
};

struct QueueInfo {
    uint32_t qid;
    uint64_t db_offset;
};

enum class ClientError {
    OK,
    NOT_FOUND,
    INVALID_ARGUMENT,
    PERMISSION_DENIED,
    RESOURCE_EXHAUSTED,
    DEADLINE_EXCEEDED,
    FAILED_PRECONDITION,
    INTERNAL,
    IO_ERROR,
    PROTOCOL_ERROR,
};

} // namespace storage_lender
