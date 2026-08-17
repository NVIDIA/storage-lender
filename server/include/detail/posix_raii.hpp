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

#include <utility>

#include <sys/stat.h>
#include <unistd.h>

namespace storage_lender::server_detail {
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept
        : fd_ { fd }
    {
    }

    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept
        : fd_ { other.release() }
    {
    }

    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    explicit operator bool() const noexcept { return fd_ >= 0; }
    int get() const noexcept { return fd_; }
    int release() noexcept { return std::exchange(fd_, -1); }

    void reset(int fd = -1) noexcept
    {
        if (fd_ == fd) {
            return;
        }
        const auto previous = std::exchange(fd_, fd);
        if (previous >= 0) {
            ::close(previous);
        }
    }

private:
    int fd_ { -1 };
};

class ScopedUmask {
public:
    explicit ScopedUmask(mode_t mask) noexcept
        : previous_ { ::umask(mask) }
    {
    }

    ~ScopedUmask() { ::umask(previous_); }

    ScopedUmask(const ScopedUmask&) = delete;
    ScopedUmask& operator=(const ScopedUmask&) = delete;
    ScopedUmask(ScopedUmask&&) = delete;
    ScopedUmask& operator=(ScopedUmask&&) = delete;

private:
    mode_t previous_;
};
}
