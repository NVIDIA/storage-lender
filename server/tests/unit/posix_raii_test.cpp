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

#include "detail/posix_raii.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using storage_lender::server_detail::ScopedUmask;
using storage_lender::server_detail::UniqueFd;

namespace {
std::array<UniqueFd, 2> make_pipe()
{
    std::array<int, 2> descriptors { -1, -1 };
    if (::pipe2(descriptors.data(), O_CLOEXEC) != 0) {
        return { };
    }
    return { UniqueFd { descriptors[0] }, UniqueFd { descriptors[1] } };
}

bool descriptor_is_open(int fd) { return ::fcntl(fd, F_GETFD) >= 0; }

mode_t current_umask()
{
    const auto current = ::umask(0);
    ::umask(current);
    return current;
}
}

TEST(UniqueFdTest, DefaultConstructionIsInvalid)
{
    const UniqueFd fd;
    EXPECT_FALSE(fd);
    EXPECT_EQ(fd.get(), -1);
}

TEST(UniqueFdTest, DestructionClosesOwnedDescriptor)
{
    int raw_fd = -1;
    {
        auto descriptors = make_pipe();
        ASSERT_TRUE(descriptors[0]);
        raw_fd = descriptors[0].get();
    }

    errno = 0;
    EXPECT_FALSE(descriptor_is_open(raw_fd));
    EXPECT_EQ(errno, EBADF);
}

TEST(UniqueFdTest, MoveConstructionTransfersOwnership)
{
    auto descriptors = make_pipe();
    ASSERT_TRUE(descriptors[0]);
    const auto raw_fd = descriptors[0].get();

    UniqueFd moved { std::move(descriptors[0]) };

    EXPECT_FALSE(descriptors[0]);
    EXPECT_EQ(moved.get(), raw_fd);
    EXPECT_TRUE(descriptor_is_open(raw_fd));
}

TEST(UniqueFdTest, MoveAssignmentClosesOldAndTransfersNewDescriptor)
{
    auto descriptors = make_pipe();
    ASSERT_TRUE(descriptors[0]);
    ASSERT_TRUE(descriptors[1]);
    const auto source_fd = descriptors[0].get();
    const auto replaced_fd = descriptors[1].get();

    descriptors[1] = std::move(descriptors[0]);

    EXPECT_FALSE(descriptors[0]);
    EXPECT_EQ(descriptors[1].get(), source_fd);
    errno = 0;
    EXPECT_FALSE(descriptor_is_open(replaced_fd));
    EXPECT_EQ(errno, EBADF);
}

TEST(UniqueFdTest, ResetClosesOldAndAdoptsNewDescriptor)
{
    auto descriptors = make_pipe();
    ASSERT_TRUE(descriptors[0]);
    ASSERT_TRUE(descriptors[1]);
    const auto old_fd = descriptors[0].get();
    const auto new_fd = descriptors[1].release();

    descriptors[0].reset(new_fd);

    EXPECT_EQ(descriptors[0].get(), new_fd);
    errno = 0;
    EXPECT_FALSE(descriptor_is_open(old_fd));
    EXPECT_EQ(errno, EBADF);
}

TEST(UniqueFdTest, ResettingCurrentDescriptorIsNoOp)
{
    auto descriptors = make_pipe();
    ASSERT_TRUE(descriptors[0]);
    const auto raw_fd = descriptors[0].get();

    descriptors[0].reset(raw_fd);

    EXPECT_EQ(descriptors[0].get(), raw_fd);
    EXPECT_TRUE(descriptor_is_open(raw_fd));
}

TEST(UniqueFdTest, ReleaseReturnsDescriptorWithoutClosingIt)
{
    auto descriptors = make_pipe();
    ASSERT_TRUE(descriptors[0]);
    const auto raw_fd = descriptors[0].release();

    EXPECT_FALSE(descriptors[0]);
    EXPECT_TRUE(descriptor_is_open(raw_fd));
    EXPECT_EQ(::close(raw_fd), 0);
}

TEST(ScopedUmaskTest, AppliesAndRestoresProcessMask)
{
    const auto original = current_umask();
    {
        ScopedUmask mask { 0027 };
        EXPECT_EQ(current_umask(), 0027);
    }
    EXPECT_EQ(current_umask(), original);
}
