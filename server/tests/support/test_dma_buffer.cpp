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

#include "test_dma_buffer.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>

std::size_t system_page_size()
{
    const auto page_size = ::sysconf(_SC_PAGESIZE);
    return page_size > 0 ? static_cast<std::size_t>(page_size) : 4096;
}

std::expected<UniqueFd, int> make_unsealed_memfd(std::size_t size)
{
    UniqueFd fd { ::memfd_create("storage-lender-test", MFD_CLOEXEC | MFD_ALLOW_SEALING) };
    if (!fd) {
        return std::unexpected(errno);
    }
    if (::ftruncate(fd.get(), static_cast<off_t>(size)) != 0) {
        const auto error = errno;
        return std::unexpected(error);
    }
    return fd;
}

std::expected<UniqueFd, int> make_sealed_memfd(std::size_t size)
{
    auto fd = make_unsealed_memfd(size);
    if (!fd) {
        return std::unexpected(fd.error());
    }
    if (::fcntl(fd->get(), F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) != 0) {
        const auto error = errno;
        return std::unexpected(error);
    }
    return fd;
}
