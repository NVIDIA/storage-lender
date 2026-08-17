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

#include "storage_lender/client.hpp"

#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

using storage_lender::ClientError;
using storage_lender::DeviceInfo;
using storage_lender::OpenDeviceMode;
using storage_lender::QueueInfo;
using storage_lender::StorageLenderClient;

namespace {

constexpr const char* SOCKET_PATH = "/run/storage-lender/api.sock";
constexpr uint32_t REQUESTED_QUEUE_ENTRIES = 64;
constexpr uint64_t CQE_SIZE = 16;
constexpr uint64_t SQE_SIZE = 64;
constexpr uint64_t HUGE_PAGE_SIZE = 2U * 1024U * 1024U;

struct QueueBuffer {
    void* address = MAP_FAILED;
    uint64_t iova = 0;
    uint64_t allocation_size = 0;
    uint64_t mapping_size = 0;
    uint64_t queue_bytes = 0;
    bool server_mapped = false;
};

uint64_t round_up(uint64_t value, uint64_t alignment) { return ((value + alignment - 1) / alignment) * alignment; }

const char* client_error_name(ClientError error)
{
    switch (error) {
    case ClientError::OK:
        return "OK";
    case ClientError::NOT_FOUND:
        return "NOT_FOUND";
    case ClientError::INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case ClientError::PERMISSION_DENIED:
        return "PERMISSION_DENIED";
    case ClientError::RESOURCE_EXHAUSTED:
        return "RESOURCE_EXHAUSTED";
    case ClientError::DEADLINE_EXCEEDED:
        return "DEADLINE_EXCEEDED";
    case ClientError::FAILED_PRECONDITION:
        return "FAILED_PRECONDITION";
    case ClientError::INTERNAL:
        return "INTERNAL";
    case ClientError::IO_ERROR:
        return "IO_ERROR";
    case ClientError::PROTOCOL_ERROR:
        return "PROTOCOL_ERROR";
    }
    return "UNKNOWN";
}

bool create_queue_buffer(
    StorageLenderClient& client, const char* name, uint64_t queue_bytes, uint64_t system_page_size, QueueBuffer& buffer)
{
    buffer.queue_bytes = queue_bytes;
    buffer.mapping_size = round_up(queue_bytes, system_page_size);
    buffer.allocation_size = round_up(buffer.mapping_size, HUGE_PAGE_SIZE);

    int memfd = ::memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_HUGETLB | MFD_HUGE_2MB);
    if (memfd < 0) {
        ::perror("memfd_create");
        return false;
    }
    if (::ftruncate(memfd, static_cast<off_t>(buffer.allocation_size)) != 0) {
        ::perror("ftruncate");
        ::close(memfd);
        return false;
    }

    buffer.address = ::mmap(nullptr, buffer.allocation_size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (buffer.address == MAP_FAILED) {
        ::perror("mmap queue buffer");
        ::close(memfd);
        return false;
    }
    ::memset(buffer.address, 0, buffer.allocation_size);

    if (::fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) != 0) {
        ::perror("fcntl(F_ADD_SEALS)");
        ::munmap(buffer.address, buffer.allocation_size);
        buffer.address = MAP_FAILED;
        ::close(memfd);
        return false;
    }

    uint32_t fd_id = 0;
    auto error = client.transfer_fd(memfd, fd_id);
    ::close(memfd);
    if (error != ClientError::OK) {
        ::fprintf(stderr, "error: transfer_fd(%s): %s\n", name, client_error_name(error));
        ::munmap(buffer.address, buffer.allocation_size);
        buffer.address = MAP_FAILED;
        return false;
    }

    error = client.map_buffer(fd_id, buffer.mapping_size, buffer.iova);
    if (error != ClientError::OK) {
        ::fprintf(stderr, "error: map_buffer(%s): %s\n", name, client_error_name(error));
        ::munmap(buffer.address, buffer.allocation_size);
        buffer.address = MAP_FAILED;
        return false;
    }
    buffer.server_mapped = true;

    ::printf("Mapped %s: iova=0x%" PRIx64 ", queue=%" PRIu64 " B, mapping=%" PRIu64 " B, backing=%" PRIu64 " B\n", name,
        buffer.iova, buffer.queue_bytes, buffer.mapping_size, buffer.allocation_size);
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 2) {
        ::fprintf(stderr, "Usage: %s <pci-address>\n", argv[0]);
        ::fprintf(stderr, "Example: %s 0000:03:00.0\n", argv[0]);
        return 1;
    }

    const char* const pci_address = argv[1];

    StorageLenderClient client;
    auto error = StorageLenderClient::connect(SOCKET_PATH, client);
    if (error != ClientError::OK) {
        ::fprintf(stderr, "error: connect(%s): %s\n", SOCKET_PATH, client_error_name(error));
        return 1;
    }
    ::printf("Connected to %s\n", SOCKET_PATH);

    uint32_t device_id = 0;
    bool device_open = false;
    QueueBuffer cq_buffer;
    QueueBuffer sq_buffer;
    QueueInfo cq { };
    QueueInfo sq { };
    bool cq_created = false;
    bool sq_created = false;
    void* bar0 = MAP_FAILED;
    size_t bar0_length = 0;

    auto cleanup = [&] {
        bool success = true;

        if (bar0 != MAP_FAILED) {
            if (::munmap(bar0, bar0_length) != 0) {
                ::perror("munmap bar0");
                success = false;
            }
            bar0 = MAP_FAILED;
        }
        if (sq_created) {
            error = client.delete_sq(sq.qid);
            if (error != ClientError::OK) {
                ::fprintf(stderr, "error: delete_sq: %s\n", client_error_name(error));
                success = false;
            }
            sq_created = false;
        }
        if (cq_created) {
            error = client.delete_cq(cq.qid);
            if (error != ClientError::OK) {
                ::fprintf(stderr, "error: delete_cq: %s\n", client_error_name(error));
                success = false;
            }
            cq_created = false;
        }
        if (device_open) {
            error = client.close_device(device_id);
            if (error != ClientError::OK) {
                ::fprintf(stderr, "error: close_device: %s\n", client_error_name(error));
                success = false;
            }
            device_open = false;
        }

        auto release_buffer = [&](QueueBuffer& buffer, const char* name) {
            if (buffer.server_mapped) {
                error = client.unmap_buffer(buffer.iova);
                if (error != ClientError::OK) {
                    ::fprintf(stderr, "error: unmap_buffer(%s): %s\n", name, client_error_name(error));
                    success = false;
                }
                buffer.server_mapped = false;
            }
            if (buffer.address != MAP_FAILED) {
                if (::munmap(buffer.address, buffer.allocation_size) != 0) {
                    ::perror("munmap queue buffer");
                    success = false;
                }
                buffer.address = MAP_FAILED;
            }
        };

        release_buffer(sq_buffer, "SQ buffer");
        release_buffer(cq_buffer, "CQ buffer");
        return success;
    };

    error = client.open_device(pci_address, OpenDeviceMode::SHARED, device_id);
    if (error != ClientError::OK) {
        ::fprintf(stderr, "error: open_device(%s): %s\n", pci_address, client_error_name(error));
        return 1;
    }
    device_open = true;
    ::printf("Opened %s (device_id=%u)\n", pci_address, device_id);

    DeviceInfo info;
    error = client.get_device_info(device_id, info);
    if (error != ClientError::OK) {
        ::fprintf(stderr, "error: get_device_info: %s\n", client_error_name(error));
        cleanup();
        return 1;
    }
    ::printf("  Model:             %s\n", info.model.c_str());
    ::printf("  BAR0 resource:     %s\n", info.pci_resource_path.c_str());
    ::printf("  Page size:         %u B\n", info.page_size);
    ::printf("  Max queue entries: %u\n", info.max_queue_entries);
    ::printf("  I/O queues:        %u\n", info.num_io_queues);
    std::for_each(info.namespaces.begin(), info.namespaces.end(), [](const auto& ns) {
        ::printf("  Namespace %u: %u B/block, %" PRIu64 " blocks\n", ns.ns_id, ns.block_size, ns.block_count);
    });

    const uint32_t queue_size = std::min(REQUESTED_QUEUE_ENTRIES, info.max_queue_entries);
    if (queue_size < 2) {
        ::fprintf(stderr, "error: controller does not support a usable I/O queue\n");
        cleanup();
        return 1;
    }

    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        ::perror("sysconf(_SC_PAGESIZE)");
        cleanup();
        return 1;
    }

    const uint64_t cq_bytes = static_cast<uint64_t>(queue_size) * CQE_SIZE;
    const uint64_t sq_bytes = static_cast<uint64_t>(queue_size) * SQE_SIZE;
    if (!create_queue_buffer(client, "storage-lender-cq", cq_bytes, static_cast<uint64_t>(page_size), cq_buffer)
        || !create_queue_buffer(client, "storage-lender-sq", sq_bytes, static_cast<uint64_t>(page_size), sq_buffer)) {
        cleanup();
        return 1;
    }

    error = client.create_cq(device_id, cq_buffer.iova, queue_size, cq);
    if (error != ClientError::OK) {
        ::fprintf(stderr, "error: create_cq: %s\n", client_error_name(error));
        cleanup();
        return 1;
    }
    cq_created = true;
    ::printf("Created CQ %u (doorbell offset=0x%" PRIx64 ")\n", cq.qid, cq.db_offset);

    error = client.create_sq(device_id, cq.qid, sq_buffer.iova, queue_size, sq);
    if (error != ClientError::OK) {
        ::fprintf(stderr, "error: create_sq: %s\n", client_error_name(error));
        cleanup();
        return 1;
    }
    sq_created = true;
    ::printf("Created SQ %u (doorbell offset=0x%" PRIx64 ")\n", sq.qid, sq.db_offset);

    int bar0_fd = ::open(info.pci_resource_path.c_str(), O_RDWR | O_SYNC | O_CLOEXEC);
    if (bar0_fd < 0) {
        ::perror("open bar0");
        cleanup();
        return 1;
    }
    bar0_length = static_cast<size_t>(std::max(cq.db_offset, sq.db_offset) + sizeof(uint32_t));
    bar0 = ::mmap(nullptr, bar0_length, PROT_READ | PROT_WRITE, MAP_SHARED, bar0_fd, 0);
    ::close(bar0_fd);
    if (bar0 == MAP_FAILED) {
        ::perror("mmap bar0");
        cleanup();
        return 1;
    }

    ::printf("\nProvisioned one queue pair and mapped its BAR0 doorbells.\n");
    ::printf("This example does not submit NVMe commands or ring a doorbell.\n");

    if (!cleanup()) {
        return 1;
    }
    ::printf("Clean teardown completed.\n");
    return 0;
}
