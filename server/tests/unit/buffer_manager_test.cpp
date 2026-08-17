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
#include "mock_nvme_backend.hpp"
#include "quota_manager.hpp"
#include "test_dma_buffer.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <expected>
#include <utility>

using ::testing::_;
using ::testing::Return;

static bool is_udmabuf_permission_error(int error) { return error == EACCES || error == EPERM; }

static std::expected<UniqueFd, int> make_udmabuf(size_t size)
{
    auto memfd = make_sealed_memfd(size);
    if (!memfd) {
        return std::unexpected(memfd.error());
    }

    UniqueFd device_fd { ::open("/dev/udmabuf", O_RDWR | O_CLOEXEC) };
    if (!device_fd) {
        const auto error = errno;
        return std::unexpected(error);
    }

    struct udmabuf_create create {
        .memfd = static_cast<__u32>(memfd->get()),
        .flags = UDMABUF_FLAGS_CLOEXEC,
        .offset = 0,
        .size = size,
    };
    UniqueFd fd { ::ioctl(device_fd.get(), UDMABUF_CREATE, &create) };
    const auto error = errno;
    if (!fd) {
        return std::unexpected(error);
    }
    return fd;
}

TEST(UdmabufTest, OnlyPermissionErrorsAreSkippable)
{
    EXPECT_TRUE(is_udmabuf_permission_error(EACCES));
    EXPECT_TRUE(is_udmabuf_permission_error(EPERM));
    EXPECT_FALSE(is_udmabuf_permission_error(ENOENT));
}

class BufferManagerTest : public ::testing::Test {
protected:
    QuotaLease fd_lease() { return std::move(*quotas_.acquire("test", QuotaAmounts { .transferred_fds = 1 })); }

    QuotaLease buffer_lease(uint64_t size) { return std::move(*quotas_.acquire_buffer("test", size)); }

    std::expected<uint32_t, LenderError> register_fd(UniqueFd fd)
    {
        auto size = BufferManager::validate_fd(fd.get());
        if (!size) {
            return std::unexpected(size.error());
        }
        return bm_.register_fd(fd.release(), *size, fd_lease());
    }

    ::testing::NiceMock<MockNvmeBackend> mock_;
    QuotaManager quotas_ { QuotaPolicy { } };
    BufferManager bm_ { mock_ };
};

TEST_F(BufferManagerTest, RegisterFdReturnsFdId)
{
    auto fd = make_sealed_memfd(4096);
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
    auto result = register_fd(std::move(*fd));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1u);
}

TEST_F(BufferManagerTest, RegisterFdMonotonicallyIncrementsId)
{
    auto fd1 = make_sealed_memfd(4096);
    auto fd2 = make_sealed_memfd(4096);
    ASSERT_TRUE(fd1.has_value()) << "memfd creation failed (errno=" << fd1.error() << ')';
    ASSERT_TRUE(fd2.has_value()) << "memfd creation failed (errno=" << fd2.error() << ')';
    auto r1 = register_fd(std::move(*fd1));
    auto r2 = register_fd(std::move(*fd2));
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r1 + 1, *r2);
}

TEST_F(BufferManagerTest, RegisterFdRejectsUnsealedFd)
{
    auto fd = make_unsealed_memfd(4096);
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';

    auto result = BufferManager::validate_fd(fd->get());

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::INVALID_ARGUMENT);
}

TEST_F(BufferManagerTest, RegisterFdRejectsAnonInodeWithoutExportedSize)
{
    UniqueFd fd { ::eventfd(0, EFD_CLOEXEC) };
    ASSERT_TRUE(fd);

    auto result = BufferManager::validate_fd(fd.get());

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::INVALID_ARGUMENT);
}

TEST_F(BufferManagerTest, RegisterFdAcceptsDmaBufWithoutSeals)
{
    auto fd = make_udmabuf(system_page_size());
    if (!fd && is_udmabuf_permission_error(fd.error())) {
        GTEST_SKIP() << "/dev/udmabuf access denied (errno=" << fd.error() << ')';
    }
    ASSERT_TRUE(fd.has_value()) << "udmabuf creation failed (errno=" << fd.error() << ')';

    auto result = register_fd(std::move(*fd));

    ASSERT_TRUE(result.has_value());
}

TEST_F(BufferManagerTest, RegisterFdRejectsNonPageMultipleFdSize)
{
    auto fd = make_sealed_memfd(system_page_size() + 1);
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';

    auto result = BufferManager::validate_fd(fd->get());

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::INVALID_ARGUMENT);
}

TEST_F(BufferManagerTest, MapBufferRegistersAndReturnsIova)
{
    auto fd = make_sealed_memfd(4096);
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
    auto fd_id = register_fd(std::move(*fd));
    ASSERT_TRUE(fd_id.has_value());

    EXPECT_CALL(mock_, mem_register_dma_buf(_, 4096)).WillOnce(Return(std::expected<uint64_t, int> { 0x1000 }));

    auto result = bm_.map_buffer(*fd_id, 4096, 0, buffer_lease(4096));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x1000u);
    EXPECT_TRUE(bm_.has_iova(*result));

    auto sz = bm_.buf_size(*result);
    ASSERT_TRUE(sz.has_value());
    EXPECT_EQ(*sz, 4096u);
}

TEST_F(BufferManagerTest, MapBufferFailsWhenSizeExceedsBackingFd)
{
    auto fd = make_sealed_memfd(4096);
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
    auto fd_id = register_fd(std::move(*fd));
    ASSERT_TRUE(fd_id.has_value());

    EXPECT_CALL(mock_, mem_register_dma_buf(_, _)).Times(0);

    auto result = bm_.map_buffer(*fd_id, 8192, 0, buffer_lease(8192));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::INVALID_ARGUMENT);
}

TEST_F(BufferManagerTest, MapBufferFailsWhenSizeIsNotPageMultiple)
{
    auto fd = make_sealed_memfd(system_page_size() * 2);
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
    auto fd_id = register_fd(std::move(*fd));
    ASSERT_TRUE(fd_id.has_value());

    EXPECT_CALL(mock_, mem_register_dma_buf(_, _)).Times(0);

    auto result = bm_.map_buffer(*fd_id, system_page_size() + 1, 0, buffer_lease(system_page_size() + 1));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::INVALID_ARGUMENT);
}

TEST_F(BufferManagerTest, MapBufferFailsOnRegisterError)
{
    auto fd = make_sealed_memfd(4096);
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
    auto fd_id = register_fd(std::move(*fd));
    ASSERT_TRUE(fd_id.has_value());

    EXPECT_CALL(mock_, mem_register_dma_buf(_, _)).WillOnce(Return(std::unexpected(-1)));
    auto result = bm_.map_buffer(*fd_id, 4096, 0, buffer_lease(4096));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::INTERNAL);
}

TEST_F(BufferManagerTest, MapBufferNotFoundOnBadFdId)
{
    auto result = bm_.map_buffer(999, 4096, 0, buffer_lease(4096));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::NOT_FOUND);
}

TEST_F(BufferManagerTest, UnmapBufferCallsUnregister)
{
    auto fd = make_sealed_memfd(4096);
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
    auto fd_id = register_fd(std::move(*fd));
    ASSERT_TRUE(fd_id.has_value());

    EXPECT_CALL(mock_, mem_register_dma_buf(_, 4096)).WillOnce(Return(std::expected<uint64_t, int> { 0x2000 }));
    EXPECT_CALL(mock_, mem_unregister_dma_buf(_, 4096)).WillOnce(Return(std::expected<void, int> { }));

    auto map_result = bm_.map_buffer(*fd_id, 4096, 0, buffer_lease(4096));
    ASSERT_TRUE(map_result.has_value());

    auto result = bm_.unmap_buffer(*map_result);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(bm_.has_iova(*map_result));
}

TEST_F(BufferManagerTest, UnmapBufferNotFoundOnBadIova)
{
    auto result = bm_.unmap_buffer(0xDEAD);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::NOT_FOUND);
}

TEST_F(BufferManagerTest, BufSizeNotFoundOnBadIova)
{
    auto result = bm_.buf_size(0xDEAD);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::NOT_FOUND);
}

TEST_F(BufferManagerTest, MapBufferFailsOnZeroSize)
{
    auto fd = make_sealed_memfd(4096);
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
    auto fd_id = register_fd(std::move(*fd));
    ASSERT_TRUE(fd_id.has_value());

    auto result = bm_.map_buffer(*fd_id, 0, 0, buffer_lease(0));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::INVALID_ARGUMENT);
}

TEST_F(BufferManagerTest, ValidatedFdRetainsTransferredFdLease)
{
    auto fd = make_sealed_memfd(system_page_size());
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
    const auto before = quotas_.snapshot();

    auto size = BufferManager::validate_fd(fd->get());
    ASSERT_TRUE(size.has_value());
    auto fd_id = bm_.register_fd(fd->release(), *size, fd_lease());

    ASSERT_TRUE(fd_id.has_value());
    const auto after = quotas_.snapshot();
    EXPECT_EQ(before.global_usage, UsageTotals { });
    EXPECT_EQ(after.global_usage.active, (QuotaAmounts { .transferred_fds = 1 }));
    EXPECT_EQ(after.global_usage.orphan, QuotaAmounts { });
    EXPECT_EQ(after.principal_usage.at("test"), after.global_usage);
}

TEST_F(BufferManagerTest, FailedFdValidationNeverRetainsLeaseOrFd)
{
    auto fd = make_unsealed_memfd(system_page_size());
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
    const auto before = quotas_.snapshot();

    auto size = BufferManager::validate_fd(fd->get());

    ASSERT_FALSE(size.has_value());
    EXPECT_EQ(size.error(), LenderError::INVALID_ARGUMENT);
    const auto after = quotas_.snapshot();
    EXPECT_EQ(before.global_usage, UsageTotals { });
    EXPECT_EQ(after.global_usage, before.global_usage);
    EXPECT_EQ(after.principal_usage, before.principal_usage);
    EXPECT_EQ(::fcntl(fd->get(), F_GETFD), FD_CLOEXEC);
}

TEST_F(BufferManagerTest, SuccessfulMapReleasesFdLeaseAndRetainsBufferLease)
{
    auto fd = make_sealed_memfd(system_page_size());
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
    const auto raw_fd = fd->get();
    auto fd_id = register_fd(std::move(*fd));
    ASSERT_TRUE(fd_id.has_value());
    auto mapping_lease = buffer_lease(system_page_size());
    const auto before = quotas_.snapshot();
    EXPECT_CALL(mock_, mem_register_dma_buf(raw_fd, system_page_size()))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x3000 }));

    auto result = bm_.map_buffer(*fd_id, system_page_size(), 0, std::move(mapping_lease));

    ASSERT_TRUE(result.has_value());
    const auto after = quotas_.snapshot();
    EXPECT_EQ(before.global_usage,
        (UsageTotals {
            .active = QuotaAmounts { .transferred_fds = 1, .mapped_buffers = 1, .mapped_bytes = system_page_size() },
            .orphan = { },
        }));
    EXPECT_EQ(after.global_usage.active, (QuotaAmounts { .mapped_buffers = 1, .mapped_bytes = system_page_size() }));
    EXPECT_EQ(after.global_usage.orphan, QuotaAmounts { });
    EXPECT_EQ(after.principal_usage.at("test"), after.global_usage);
}

TEST_F(BufferManagerTest, FailedDmaRegistrationRetainsFdLeaseAndReleasesBufferLease)
{
    auto fd = make_sealed_memfd(system_page_size());
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
    const auto raw_fd = fd->get();
    auto fd_id = register_fd(std::move(*fd));
    ASSERT_TRUE(fd_id.has_value());
    auto mapping_lease = buffer_lease(system_page_size());
    const auto before = quotas_.snapshot();
    EXPECT_CALL(mock_, mem_register_dma_buf(raw_fd, system_page_size())).WillOnce(Return(std::unexpected(-1)));

    auto result = bm_.map_buffer(*fd_id, system_page_size(), 0, std::move(mapping_lease));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::INTERNAL);
    const auto after = quotas_.snapshot();
    EXPECT_EQ(before.global_usage,
        (UsageTotals {
            .active = QuotaAmounts { .transferred_fds = 1, .mapped_buffers = 1, .mapped_bytes = system_page_size() },
            .orphan = { },
        }));
    EXPECT_EQ(after.global_usage.active, (QuotaAmounts { .transferred_fds = 1 }));
    EXPECT_EQ(after.global_usage.orphan, QuotaAmounts { });
    EXPECT_EQ(after.principal_usage.at("test"), after.global_usage);
}

TEST_F(BufferManagerTest, SuccessfulUnmapReleasesMappedCountAndBytes)
{
    auto fd = make_sealed_memfd(system_page_size());
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
    const auto raw_fd = fd->get();
    auto fd_id = register_fd(std::move(*fd));
    ASSERT_TRUE(fd_id.has_value());
    EXPECT_CALL(mock_, mem_register_dma_buf(raw_fd, system_page_size()))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x4000 }));
    auto iova = bm_.map_buffer(*fd_id, system_page_size(), 0, buffer_lease(system_page_size()));
    ASSERT_TRUE(iova.has_value());
    const auto before = quotas_.snapshot();
    EXPECT_CALL(mock_, mem_unregister_dma_buf(*iova, system_page_size()))
        .WillOnce(Return(std::expected<void, int> { }));

    auto result = bm_.unmap_buffer(*iova);

    ASSERT_TRUE(result.has_value());
    const auto after = quotas_.snapshot();
    EXPECT_EQ(before.global_usage,
        (UsageTotals {
            .active = QuotaAmounts { .mapped_buffers = 1, .mapped_bytes = system_page_size() },
            .orphan = { },
        }));
    EXPECT_EQ(after.global_usage, UsageTotals { });
    EXPECT_TRUE(after.principal_usage.empty());
}

TEST_F(BufferManagerTest, FailedExplicitUnmapRetainsEntryAndBufferLeaseForRetry)
{
    auto fd = make_sealed_memfd(system_page_size());
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
    const auto raw_fd = fd->get();
    auto fd_id = register_fd(std::move(*fd));
    ASSERT_TRUE(fd_id.has_value());
    EXPECT_CALL(mock_, mem_register_dma_buf(raw_fd, system_page_size()))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x5000 }));
    auto iova = bm_.map_buffer(*fd_id, system_page_size(), 0, buffer_lease(system_page_size()));
    ASSERT_TRUE(iova.has_value());
    const auto before = quotas_.snapshot();
    EXPECT_CALL(mock_, mem_unregister_dma_buf(*iova, system_page_size())).WillOnce(Return(std::unexpected(-1)));

    auto failed = bm_.unmap_buffer(*iova);

    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error(), LenderError::INTERNAL);
    const auto after = quotas_.snapshot();
    EXPECT_EQ(before.global_usage,
        (UsageTotals {
            .active = QuotaAmounts { .mapped_buffers = 1, .mapped_bytes = system_page_size() },
            .orphan = { },
        }));
    EXPECT_EQ(after.global_usage, before.global_usage);
    EXPECT_EQ(after.principal_usage, before.principal_usage);
    EXPECT_TRUE(bm_.has_iova(*iova));

    EXPECT_CALL(mock_, mem_unregister_dma_buf(*iova, system_page_size()))
        .WillOnce(Return(std::expected<void, int> { }));
    EXPECT_TRUE(bm_.unmap_buffer(*iova).has_value());
}

TEST_F(BufferManagerTest, CleanupOrphansMappingWhenDmaUnregisterFails)
{
    auto fd = make_sealed_memfd(system_page_size());
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
    const auto raw_fd = fd->get();
    auto fd_id = register_fd(std::move(*fd));
    ASSERT_TRUE(fd_id.has_value());
    EXPECT_CALL(mock_, mem_register_dma_buf(raw_fd, system_page_size()))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x6000 }));
    auto iova = bm_.map_buffer(*fd_id, system_page_size(), 0, buffer_lease(system_page_size()));
    ASSERT_TRUE(iova.has_value());
    const auto before = quotas_.snapshot();
    EXPECT_CALL(mock_, mem_unregister_dma_buf(*iova, system_page_size())).WillOnce(Return(std::unexpected(-1)));

    bm_.cleanup_all();

    const auto after = quotas_.snapshot();
    EXPECT_EQ(before.global_usage,
        (UsageTotals {
            .active = QuotaAmounts { .mapped_buffers = 1, .mapped_bytes = system_page_size() },
            .orphan = { },
        }));
    EXPECT_EQ(after.global_usage.active, QuotaAmounts { });
    EXPECT_EQ(after.global_usage.orphan, (QuotaAmounts { .mapped_buffers = 1, .mapped_bytes = system_page_size() }));
    EXPECT_EQ(after.principal_usage.at("test"), after.global_usage);
    EXPECT_FALSE(bm_.has_iova(*iova));
    bm_.cleanup_all();
}

TEST_F(BufferManagerTest, CleanupReleasesUnusedTransferredFdLeaseAfterClose)
{
    auto fd = make_sealed_memfd(system_page_size());
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
    const auto raw_fd = fd->get();
    auto fd_id = register_fd(std::move(*fd));
    ASSERT_TRUE(fd_id.has_value());
    const auto before = quotas_.snapshot();

    bm_.cleanup_all();

    const auto after = quotas_.snapshot();
    EXPECT_EQ(before.global_usage, (UsageTotals { .active = QuotaAmounts { .transferred_fds = 1 }, .orphan = { } }));
    EXPECT_EQ(after.global_usage, UsageTotals { });
    EXPECT_TRUE(after.principal_usage.empty());
    EXPECT_EQ(::fcntl(raw_fd, F_GETFD), -1);
    bm_.cleanup_all();
}

TEST_F(BufferManagerTest, DuplicateIovaOrphansAttemptedMappingAndRetainsFdLease)
{
    auto first_fd = make_sealed_memfd(system_page_size());
    auto second_fd = make_sealed_memfd(system_page_size());
    ASSERT_TRUE(first_fd.has_value()) << "memfd creation failed (errno=" << first_fd.error() << ')';
    ASSERT_TRUE(second_fd.has_value()) << "memfd creation failed (errno=" << second_fd.error() << ')';
    const auto first_raw_fd = first_fd->get();
    const auto second_raw_fd = second_fd->get();
    auto first_fd_id = register_fd(std::move(*first_fd));
    auto second_fd_id = register_fd(std::move(*second_fd));
    ASSERT_TRUE(first_fd_id.has_value());
    ASSERT_TRUE(second_fd_id.has_value());
    EXPECT_CALL(mock_, mem_register_dma_buf(first_raw_fd, system_page_size()))
        .WillOnce(Return(std::expected<uint64_t, int> { 0x7000 }));
    auto first_iova = bm_.map_buffer(*first_fd_id, system_page_size(), 0, buffer_lease(system_page_size()));
    ASSERT_TRUE(first_iova.has_value());
    auto attempted_lease = buffer_lease(system_page_size());
    const auto before = quotas_.snapshot();
    EXPECT_CALL(mock_, mem_register_dma_buf(second_raw_fd, system_page_size()))
        .WillOnce(Return(std::expected<uint64_t, int> { *first_iova }));

    auto duplicate = bm_.map_buffer(*second_fd_id, system_page_size(), 0, std::move(attempted_lease));

    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error(), LenderError::INTERNAL);
    const auto after = quotas_.snapshot();
    EXPECT_EQ(before.global_usage,
        (UsageTotals {
            .active
            = QuotaAmounts { .transferred_fds = 1, .mapped_buffers = 2, .mapped_bytes = system_page_size() * 2 },
            .orphan = { },
        }));
    EXPECT_EQ(after.global_usage.active,
        (QuotaAmounts { .transferred_fds = 1, .mapped_buffers = 1, .mapped_bytes = system_page_size() }));
    EXPECT_EQ(after.global_usage.orphan, (QuotaAmounts { .mapped_buffers = 1, .mapped_bytes = system_page_size() }));
    EXPECT_TRUE(bm_.has_iova(*first_iova));

    EXPECT_CALL(mock_, mem_unregister_dma_buf(*first_iova, system_page_size()))
        .WillOnce(Return(std::expected<void, int> { }));
    bm_.cleanup_all();
}

TEST_F(BufferManagerTest, CleanupOrphansUnusedTransferredFdLeaseWhenCloseIsUnconfirmed)
{
    auto fd = make_sealed_memfd(system_page_size());
    ASSERT_TRUE(fd.has_value()) << "memfd creation failed (errno=" << fd.error() << ')';
    const auto raw_fd = fd->get();
    auto fd_id = register_fd(std::move(*fd));
    ASSERT_TRUE(fd_id.has_value());
    ASSERT_EQ(::close(raw_fd), 0);
    const auto before = quotas_.snapshot();

    bm_.cleanup_all();

    const auto after = quotas_.snapshot();
    EXPECT_EQ(before.global_usage, (UsageTotals { .active = QuotaAmounts { .transferred_fds = 1 }, .orphan = { } }));
    EXPECT_EQ(after.global_usage.active, QuotaAmounts { });
    EXPECT_EQ(after.global_usage.orphan, (QuotaAmounts { .transferred_fds = 1 }));
    EXPECT_EQ(after.principal_usage.at("test"), after.global_usage);
    bm_.cleanup_all();
}
