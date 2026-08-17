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

#include "buffer_manager.hpp"
#include "lender_error.hpp"
#include "logging.hpp"

#include <cerrno>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <utility>

namespace {

std::expected<uint64_t, LenderError> system_page_size()
{
    long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        log_error("sysconf(_SC_PAGESIZE) failed: {}", ::strerror(errno));
        return std::unexpected(LenderError::INTERNAL);
    }
    return static_cast<uint64_t>(page_size);
}

std::expected<uint64_t, LenderError> immutable_exported_size(int fd)
{
    auto size = ::lseek(fd, 0, SEEK_END);
    if (size < 0) {
        log_warn("validate_fd: fd {} does not expose an immutable size: {}", fd, ::strerror(errno));
        return std::unexpected(LenderError::INVALID_ARGUMENT);
    }
    if (size == 0) {
        log_warn("validate_fd: fd {} exposes an empty immutable size", fd);
        return std::unexpected(LenderError::INVALID_ARGUMENT);
    }
    if (::lseek(fd, 0, SEEK_SET) != 0) {
        log_warn("validate_fd: failed to reset fd {} after reading its exported size: {}", fd, ::strerror(errno));
        return std::unexpected(LenderError::INVALID_ARGUMENT);
    }
    return static_cast<uint64_t>(size);
}

std::expected<uint64_t, LenderError> sealed_regular_file_size(int fd)
{
    struct stat st { };
    if (::fstat(fd, &st) != 0) {
        log_warn("validate_fd: fstat({}) failed: {}", fd, ::strerror(errno));
        return std::unexpected(LenderError::INVALID_ARGUMENT);
    }
    if (!S_ISREG(st.st_mode)) {
        log_warn("validate_fd: fd {} is neither a regular file nor an anon-inode file", fd);
        return std::unexpected(LenderError::INVALID_ARGUMENT);
    }

    int seals = ::fcntl(fd, F_GET_SEALS);
    if (seals < 0) {
        log_warn("validate_fd: regular file fd {} does not support file seals: {}", fd, ::strerror(errno));
        return std::unexpected(LenderError::INVALID_ARGUMENT);
    }

    constexpr int REQUIRED_SEALS = F_SEAL_SHRINK | F_SEAL_GROW;
    if ((seals & REQUIRED_SEALS) != REQUIRED_SEALS) {
        log_warn("validate_fd: regular file fd {} is not sealed against resize", fd);
        return std::unexpected(LenderError::INVALID_ARGUMENT);
    }

    // Re-read the size after observing the seals. Seals cannot be removed.
    if (::fstat(fd, &st) != 0) {
        log_warn("validate_fd: fstat({}) failed: {}", fd, ::strerror(errno));
        return std::unexpected(LenderError::INVALID_ARGUMENT);
    }
    if (st.st_size <= 0) {
        log_warn("validate_fd: regular file fd {} is empty", fd);
        return std::unexpected(LenderError::INVALID_ARGUMENT);
    }
    return static_cast<uint64_t>(st.st_size);
}

} // anonymous namespace

BufferManager::BufferManager(NvmeBackend& backend)
    : backend_(backend)
{
}

BufferManager::~BufferManager() { cleanup_all(); }

std::expected<uint64_t, LenderError> BufferManager::validate_fd(int fd)
{
    struct statfs fs_info { };
    if (::fstatfs(fd, &fs_info) != 0) {
        log_warn("validate_fd: fstatfs({}) failed: {}", fd, ::strerror(errno));
        return std::unexpected(LenderError::INVALID_ARGUMENT);
    }

    bool is_dma_buf_or_anon_inode = fs_info.f_type == DMA_BUF_MAGIC || fs_info.f_type == ANON_INODE_FS_MAGIC;
    auto size = is_dma_buf_or_anon_inode ? immutable_exported_size(fd) : sealed_regular_file_size(fd);
    if (!size) {
        return std::unexpected(size.error());
    }

    auto page_size = system_page_size();
    if (!page_size) {
        return std::unexpected(page_size.error());
    }

    auto fd_size = *size;
    if ((fd_size % *page_size) != 0) {
        log_warn("validate_fd: fd {} size {} is not a multiple of system page size {}", fd, fd_size, *page_size);
        return std::unexpected(LenderError::INVALID_ARGUMENT);
    }

    return fd_size;
}

std::expected<uint32_t, LenderError> BufferManager::register_fd(int fd, uint64_t validated_size, QuotaLease quota)
{
    uint32_t fd_id = next_fd_id_++;
    received_fds_.emplace(fd_id, ReceivedFdEntry { fd, validated_size, std::move(quota) });
    return fd_id;
}

std::expected<void, LenderError> BufferManager::validate_mapping(uint32_t fd_id, uint64_t size) const
{
    auto it = received_fds_.find(fd_id);
    if (it == received_fds_.end()) {
        log_warn("map_buffer: fd id {} not found", fd_id);
        return std::unexpected(LenderError::NOT_FOUND);
    }
    if (size == 0) {
        return std::unexpected(LenderError::INVALID_ARGUMENT);
    }

    auto page_size = system_page_size();
    if (!page_size) {
        return std::unexpected(page_size.error());
    }
    if ((size % *page_size) != 0) {
        log_warn("map_buffer: size {} is not a multiple of system page size {}", size, *page_size);
        return std::unexpected(LenderError::INVALID_ARGUMENT);
    }

    if (size > it->second.size) {
        log_warn("map_buffer: size {} exceeds fd backing size {}", size, it->second.size);
        return std::unexpected(LenderError::INVALID_ARGUMENT);
    }

    return { };
}

std::expected<uint64_t, LenderError> BufferManager::map_buffer(
    uint32_t fd_id, uint64_t size, uint32_t /*alignment*/, QuotaLease quota)
{
    auto validated = validate_mapping(fd_id, size);
    if (!validated) {
        return std::unexpected(validated.error());
    }

    auto it = received_fds_.find(fd_id);
    int fd = it->second.fd;
    auto reg = backend_.mem_register_dma_buf(fd, size);
    if (!reg) {
        return std::unexpected(LenderError::INTERNAL);
    }

    auto iova = *reg;
    if (mapped_buffers_.contains(iova)) {
        log_error("map_buffer: backend returned duplicate iova {:x}; orphaning {} mapped bytes for principal {}", iova,
            size, quota.principal());
        quota.mark_orphaned();
        return std::unexpected(LenderError::INTERNAL);
    }
    mapped_buffers_.emplace(iova, MappedBufferEntry { size, fd, std::move(quota) });
    received_fds_.erase(it);
    return iova;
}

std::expected<void, LenderError> BufferManager::unmap_buffer(uint64_t iova)
{
    auto it = mapped_buffers_.find(iova);
    if (it == mapped_buffers_.end()) {
        log_warn("unmap_buffer: iova {:x} not found", iova);
        return std::unexpected(LenderError::NOT_FOUND);
    }

    auto unregistered = backend_.mem_unregister_dma_buf(iova, it->second.size);
    if (!unregistered) {
        return std::unexpected(LenderError::INTERNAL);
    }
    if (::close(it->second.fd) != 0) {
        log_error("unmap_buffer: close({}): {}", it->second.fd, ::strerror(errno));
    }
    mapped_buffers_.erase(it);
    return { };
}

bool BufferManager::has_iova(uint64_t iova) const { return mapped_buffers_.contains(iova); }

std::expected<uint64_t, LenderError> BufferManager::buf_size(uint64_t iova) const
{
    auto it = mapped_buffers_.find(iova);
    if (it == mapped_buffers_.end()) {
        log_warn("buf_size: iova {:x} not found", iova);
        return std::unexpected(LenderError::NOT_FOUND);
    }
    return it->second.size;
}

void BufferManager::cleanup_all()
{
    for (auto it = mapped_buffers_.begin(); it != mapped_buffers_.end();) {
        auto& entry = it->second;
        if (auto unregistered = backend_.mem_unregister_dma_buf(it->first, entry.size); !unregistered) {
            log_error("cleanup_all: failed to unregister {} mapped bytes for principal {}: error {}", entry.size,
                entry.quota.principal(), unregistered.error());
            entry.quota.mark_orphaned();
        }
        if (::close(entry.fd) != 0) {
            log_error("cleanup_all: close({}): {}", entry.fd, ::strerror(errno));
        }
        it = mapped_buffers_.erase(it);
    }

    for (auto it = received_fds_.begin(); it != received_fds_.end();) {
        auto& entry = it->second;
        if (::close(entry.fd) != 0) {
            log_error("cleanup_all: close({}): {}", entry.fd, ::strerror(errno));
            entry.quota.mark_orphaned();
        } else {
            log_warn("cleanup_all: closed received and unused fd {}", entry.fd);
        }
        it = received_fds_.erase(it);
    }
}
