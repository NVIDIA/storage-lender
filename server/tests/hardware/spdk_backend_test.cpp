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

// Integration tests for SpdkBackend against real NVMe hardware.
//
// Prerequisites:
//   - Must be run as root: SPDK's DPDK environment requires privileged access
//     to hugepages and PCIe resources.
//   - Hugepages must be configured, e.g.:
//       echo 64 > /proc/sys/vm/nr_hugepages
//   - Set DEVICE_BDF to the PCIe BDF of the target NVMe controller. The whole
//     suite is skipped otherwise, before SPDK is initialized.
//
// Example:
//   sudo env DEVICE_BDF=0000:01:00.0 ./server/tests/server_spdk_backend_test

#include "spdk_backend.hpp"

#include <gtest/gtest.h>

#include <spdk/env.h>
#include <spdk/memory.h>

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

// SpdkBackend initialises SPDK's DPDK environment in its constructor and tears
// it down in its destructor.  spdk_env_init / spdk_env_fini are not re-entrant
// so a single SpdkBackend instance is shared across the entire test suite via
// SetUpTestSuite / TearDownTestSuite.
//
// If SPDK cannot be initialised (insufficient privileges or no hugepages), the
// backend's constructor calls std::abort().  The binary must therefore be run
// with the required privileges and hugepages configured whenever DEVICE_BDF is
// set.
class SpdkBackendTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        if (const char* bdf = std::getenv("DEVICE_BDF")) {
            device_bdf_ = bdf;
            backend_ = std::make_unique<SpdkBackend>();
        }
    }

    static void TearDownTestSuite() { backend_.reset(); }

    void SetUp() override
    {
        if (!backend_) {
            GTEST_SKIP() << "DEVICE_BDF not set";
        }
    }

    // Connect to DEVICE_BDF, storing the handle in ctrlr_.
    void ConnectDevice()
    {
        auto r = backend_->nvme_connect(device_bdf_, connect_options_);
        ASSERT_TRUE(r.has_value()) << "nvme_connect(" << device_bdf_ << ") failed";
        ctrlr_ = *r;
    }

    // Detach ctrlr_ if a connection is open.
    void TearDown() override
    {
        if (ctrlr_) {
            EXPECT_TRUE(backend_->nvme_detach(ctrlr_).has_value());
            ctrlr_ = nullptr;
        }
    }

    static std::unique_ptr<SpdkBackend> backend_;
    static std::string device_bdf_;

    void* ctrlr_ = nullptr;
    NvmeConnectOptions connect_options_ {
        .num_io_queues = 65534,
        .admin_command_timeout = std::chrono::milliseconds { 10000 },
    };
    SleepFn sleep_fn_ { [](std::chrono::milliseconds ms) { std::this_thread::sleep_for(ms); } };
};

std::unique_ptr<SpdkBackend> SpdkBackendTest::backend_;
std::string SpdkBackendTest::device_bdf_;

// ---------------------------------------------------------------------------
// nvme_connect: error paths (no device required).
// ---------------------------------------------------------------------------

TEST_F(SpdkBackendTest, ConnectFailsOnMalformedAddress)
{
    auto r = backend_->nvme_connect("not-a-bdf", connect_options_);
    EXPECT_FALSE(r.has_value());
}

TEST_F(SpdkBackendTest, ConnectFailsOnNonExistentDevice)
{
    // Valid BDF syntax but no NVMe controller should be present here.
    auto r = backend_->nvme_connect("0000:ff:00.0", connect_options_);
    EXPECT_FALSE(r.has_value());
}

// ---------------------------------------------------------------------------
// nvme_connect / nvme_detach lifecycle.
// ---------------------------------------------------------------------------

TEST_F(SpdkBackendTest, ConnectDetach)
{
    ConnectDevice();
    ASSERT_NE(ctrlr_, nullptr);
    // TearDown() issues nvme_detach().
}

// ---------------------------------------------------------------------------
// get_controller_info.
// ---------------------------------------------------------------------------

TEST_F(SpdkBackendTest, GetControllerInfoReturnsReasonableValues)
{
    ConnectDevice();

    auto r = backend_->get_controller_info(ctrlr_);
    ASSERT_TRUE(r.has_value());

    EXPECT_FALSE(r->model.empty());
    EXPECT_EQ(r->pci_resource_path, "/sys/bus/pci/devices/" + device_bdf_ + "/resource0");
    EXPECT_GT(r->max_queue_entries, 0u);
    EXPECT_GT(r->page_size, 0u);
    EXPECT_GT(r->num_io_queues, 0u);
    // Every attached NVMe controller exposes at least one namespace.
    ASSERT_FALSE(r->namespaces.empty());
    std::ranges::for_each(r->namespaces, [](const auto& ns) {
        EXPECT_GT(ns.ns_id, 0u);
        EXPECT_GT(ns.block_size, 0u);
        EXPECT_GT(ns.block_count, 0u);
    });
}

// ---------------------------------------------------------------------------
// mem_register_dma_buf / mem_unregister_dma_buf.
//
// spdk_mem_register_dma_buf requires physically contiguous memory; use a
// hugepage-backed memfd.  The test is skipped when hugepages are unavailable.
// ---------------------------------------------------------------------------

TEST_F(SpdkBackendTest, MemRegisterUnregister)
{
    ConnectDevice();

    constexpr size_t SIZE = 2 * 1024 * 1024; // 2 MiB hugepage

#ifndef MFD_HUGETLB
#define MFD_HUGETLB 0x0004
#endif

    int fd = memfd_create("test_dma_buf", MFD_CLOEXEC | MFD_HUGETLB);
    if (fd < 0) {
        GTEST_SKIP() << "memfd_create(MFD_HUGETLB) failed (errno=" << errno << "); configure hugepages (nr_hugepages)";
    }

    if (ftruncate(fd, static_cast<off_t>(SIZE)) != 0) {
        close(fd);
        GTEST_SKIP() << "ftruncate on hugepage memfd failed (errno=" << errno << "); ensure nr_hugepages >= 1";
    }

    void* vaddr = mmap(nullptr, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_HUGETLB, fd, 0);
    if (vaddr == MAP_FAILED) {
        close(fd);
        GTEST_SKIP() << "mmap of hugepage memfd failed (errno=" << errno << ")";
    }

    auto result = backend_->mem_register_dma_buf(fd, SIZE);
    EXPECT_TRUE(result);
    EXPECT_TRUE(backend_->mem_unregister_dma_buf(*result, SIZE).has_value());

    munmap(vaddr, SIZE);
    close(fd);
}

// ---------------------------------------------------------------------------
// Completion queue lifecycle.
// ---------------------------------------------------------------------------

TEST_F(SpdkBackendTest, CreateDeleteCq)
{
    ConnectDevice();

    constexpr size_t BUF_SIZE = 4096;
    void* buf = spdk_zmalloc(BUF_SIZE, BUF_SIZE, nullptr, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
    ASSERT_NE(buf, nullptr) << "spdk_zmalloc failed";

    uint64_t iova = spdk_vtophys(buf, nullptr);
    ASSERT_NE(iova, SPDK_VTOPHYS_ERROR) << "spdk_vtophys failed";

    auto cq_r = backend_->create_cq(ctrlr_, iova, 64, sleep_fn_);
    ASSERT_TRUE(cq_r.has_value()) << "create_cq failed: " << cq_r.error();
    EXPECT_GT(cq_r->qid, 0u);

    EXPECT_TRUE(backend_->delete_cq(ctrlr_, cq_r->qid, sleep_fn_).has_value());

    spdk_free(buf);
}

// ---------------------------------------------------------------------------
// Submission queue lifecycle (requires a completion queue).
// ---------------------------------------------------------------------------

TEST_F(SpdkBackendTest, CreateDeleteSqCq)
{
    ConnectDevice();

    constexpr size_t BUF_SIZE = 4096;
    void* cq_buf = spdk_zmalloc(BUF_SIZE, BUF_SIZE, nullptr, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
    void* sq_buf = spdk_zmalloc(BUF_SIZE, BUF_SIZE, nullptr, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
    ASSERT_NE(cq_buf, nullptr);
    ASSERT_NE(sq_buf, nullptr);

    uint64_t cq_iova = spdk_vtophys(cq_buf, nullptr);
    uint64_t sq_iova = spdk_vtophys(sq_buf, nullptr);
    ASSERT_NE(cq_iova, SPDK_VTOPHYS_ERROR);
    ASSERT_NE(sq_iova, SPDK_VTOPHYS_ERROR);

    auto cq_r = backend_->create_cq(ctrlr_, cq_iova, 64, sleep_fn_);
    ASSERT_TRUE(cq_r.has_value()) << "create_cq failed: " << cq_r.error();

    auto sq_r = backend_->create_sq(ctrlr_, sq_iova, 64, cq_r->qid, sleep_fn_);
    ASSERT_TRUE(sq_r.has_value()) << "create_sq failed: " << sq_r.error();
    // SpdkBackend reuses the CQ ID as the SQ ID.
    EXPECT_EQ(sq_r->qid, cq_r->qid);

    EXPECT_TRUE(backend_->delete_sq(ctrlr_, sq_r->qid, sleep_fn_).has_value());
    EXPECT_TRUE(backend_->delete_cq(ctrlr_, cq_r->qid, sleep_fn_).has_value());

    spdk_free(cq_buf);
    spdk_free(sq_buf);
}
