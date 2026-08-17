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

#include "spdk_backend.hpp"

#include "logging.hpp"
#include "pci_address.hpp"
#include "spdk_admin_command.hpp"

#include <spdk/env.h>
#include <spdk/memory.h>
#include <spdk/nvme.h>
#include <spdk/version.h>

#include <boost/algorithm/string.hpp>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <expected>
#include <format>
#include <string>

std::string format_as(const spdk_nvme_ctrlr& ctrlr)
{
    const auto* trid = spdk_nvme_ctrlr_get_transport_id(const_cast<spdk_nvme_ctrlr*>(&ctrlr));
    switch (trid->trtype) {
    case SPDK_NVME_TRANSPORT_PCIE:
        return static_cast<const char*>(trid->traddr);
    default:
        return std::format("(unrecognized NVMe transport type {})", static_cast<int>(trid->trtype));
    }
}

std::string format_as(const spdk_nvme_cmd& cmd)
{
    switch (cmd.opc) {
    case SPDK_NVME_OPC_CREATE_IO_SQ:
        return "CREATE_IO_SQ";
    case SPDK_NVME_OPC_DELETE_IO_SQ:
        return "DELETE_IO_SQ";
    case SPDK_NVME_OPC_CREATE_IO_CQ:
        return "CREATE_IO_CQ";
    case SPDK_NVME_OPC_DELETE_IO_CQ:
        return "DELETE_IO_CQ";
    default:
        return std::format("opc={:#04x}", cmd.opc);
    }
}

SpdkBackend::SpdkBackend()
{
    struct spdk_env_opts env_opts { };
    spdk_env_opts_init(&env_opts);
    env_opts.name = "storage-lender";
    auto rc = spdk_env_init(&env_opts);
    if (rc != 0) {
        fail("Failed to initialize SPDK error {}", rc);
    }
    log_info("SPDK {}.{}.{} initialized", SPDK_VERSION_MAJOR, SPDK_VERSION_MINOR, SPDK_VERSION_PATCH);
}

SpdkBackend::~SpdkBackend()
{
    spdk_env_fini();
    log_info("SPDK {}.{}.{} finalized", SPDK_VERSION_MAJOR, SPDK_VERSION_MINOR, SPDK_VERSION_PATCH);
}

std::expected<uint64_t, int> SpdkBackend::mem_register_dma_buf(int buf_fd, size_t size)
{
    uint64_t iova = 0;
    auto rc = ::spdk_mem_register_dma_buf(buf_fd, 0, &iova, size);
    if (rc != 0) {
        log_error("Failed to register DMA buffer fd {} size {} error {}", buf_fd, size, rc);
        return std::unexpected(rc);
    }
    return iova;
}

std::expected<void, int> SpdkBackend::mem_unregister_dma_buf(uint64_t iova, size_t size)
{
    auto rc = ::spdk_mem_unregister_dma_buf(iova, size);
    if (rc != 0) {
        log_error("Failed to unregister DMA buffer iova {} size {} error {}", iova, size, rc);
        return std::unexpected(rc);
    }
    return { };
}

std::expected<void*, int> SpdkBackend::nvme_connect(const std::string& pci_address, const NvmeConnectOptions& options)
{
    if (!is_valid_pci_bdf(pci_address)) {
        log_warn("Rejected malformed PCI address {}", pci_address);
        return std::unexpected(EINVAL);
    }

    struct spdk_nvme_transport_id tid { };
    std::ostringstream tid_ss;
    tid_ss << "trtype:PCIe traddr:" << pci_address;
    auto rc = spdk_nvme_transport_id_parse(&tid, tid_ss.str().c_str());
    if (rc != 0) {
        log_warn("Failed to parse the NVMe transport id {}", tid_ss.str());
        return std::unexpected(rc);
    }

    struct spdk_nvme_ctrlr_opts opts { };
    spdk_nvme_ctrlr_get_default_ctrlr_opts(&opts, sizeof(opts));
    opts.num_io_queues = options.num_io_queues;
    log_info("Connecting NVMe device {} with num_io_queues {} admin_command_timeout_ms {}", pci_address,
        options.num_io_queues, options.admin_command_timeout.count());
    auto* ctrlr = spdk_nvme_connect(&tid, &opts, sizeof(opts));
    if (ctrlr == nullptr) {
        log_warn("Failed to connect to NVMe device {}", pci_address);
        return std::unexpected(ENODEV);
    }

    admin_command_timeouts_.insert_or_assign(ctrlr, options.admin_command_timeout);
    return ctrlr;
}

std::expected<void, int> SpdkBackend::nvme_detach(void* ctrlr_handle)
{
    auto* ctrlr = static_cast<spdk_nvme_ctrlr*>(ctrlr_handle);
    log_info("Detaching NVMe controller {}", *ctrlr);
    auto rc = spdk_nvme_detach(ctrlr);
    if (rc != 0) {
        log_error("Failed to detach NVMe controller {} error {}", *ctrlr, rc);
        return std::unexpected(rc);
    }
    admin_command_timeouts_.erase(ctrlr_handle);
    return { };
}

std::expected<NvmeBackend::ControllerInfo, int> SpdkBackend::get_controller_info(void* ctrlr_handle)
{
    auto* ctrlr = static_cast<spdk_nvme_ctrlr*>(ctrlr_handle);
    const struct spdk_nvme_ctrlr_data* cdata = spdk_nvme_ctrlr_get_data(ctrlr);

    ControllerInfo info;

    info.model = std::string(reinterpret_cast<const char*>(cdata->mn), sizeof(cdata->mn));
    boost::trim(info.model);

    const auto* trid = spdk_nvme_ctrlr_get_transport_id(ctrlr);
    info.pci_resource_path = "/sys/bus/pci/devices/" + std::string(trid->traddr) + "/resource0";

    const auto* regs = spdk_nvme_ctrlr_get_registers(ctrlr);
    info.max_queue_entries = regs->cap.bits.mqes + 1;
    info.page_size = 1u << (12u + regs->cc.bits.mps);
    info.num_io_queues = spdk_nvme_ctrlr_get_opts(ctrlr)->num_io_queues;

    for (uint32_t nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
        nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
        struct spdk_nvme_ns* ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
        if (!ns) {
            continue;
        }
        info.namespaces.emplace_back(nsid, spdk_nvme_ns_get_sector_size(ns), spdk_nvme_ns_get_num_sectors(ns));
    }

    return info;
}

std::expected<NvmeBackend::QueueInfo, int> SpdkBackend::create_cq(
    void* ctrlr_handle, uint64_t iova, uint32_t queue_size, const SleepFn& sleep_fn)
{
    auto* ctrlr = static_cast<spdk_nvme_ctrlr*>(ctrlr_handle);
    if (queue_size < 2) {
        log_warn("Invalid completion queue size {}; minimum entries 2", queue_size);
        return std::unexpected(EINVAL);
    }

    auto qid = spdk_nvme_ctrlr_alloc_qid(ctrlr);
    if (qid < 0) {
        log_error("Failed to allocate queue id controller {}", *ctrlr);
        return std::unexpected(ERANGE);
    }

    auto cq_id = static_cast<uint16_t>(qid);
    struct spdk_nvme_cmd cmd { };
    cmd.opc = SPDK_NVME_OPC_CREATE_IO_CQ;
    cmd.cdw10_bits.create_io_q.qid = cq_id;
    cmd.cdw10_bits.create_io_q.qsize = queue_size - 1; // 0 based.
    cmd.cdw11_bits.create_io_cq.pc = 1;
    cmd.dptr.prp.prp1 = iova;

    if (auto rc = execute_admin_command(ctrlr_handle, cmd, sleep_fn); rc != 0) {
        log_error("Failed to create I/O completion queue id {} controller {} error {}", cq_id, *ctrlr, rc);
        spdk_nvme_ctrlr_free_qid(ctrlr, cq_id);
        return std::unexpected(rc);
    }

    auto dstrd = spdk_nvme_ctrlr_get_registers(ctrlr)->cap.bits.dstrd;
    auto db_offset = 0x1000 + (2 * cq_id + 1) * (4u << dstrd);
    return QueueInfo { cq_id, db_offset };
}

std::expected<void, int> SpdkBackend::delete_cq(void* ctrlr_handle, uint32_t cq_id, const SleepFn& sleep_fn)
{
    auto* ctrlr = static_cast<spdk_nvme_ctrlr*>(ctrlr_handle);

    struct spdk_nvme_cmd cmd { };
    cmd.opc = SPDK_NVME_OPC_DELETE_IO_CQ;
    cmd.cdw10_bits.delete_io_q.qid = cq_id;

    if (auto rc = execute_admin_command(ctrlr_handle, cmd, sleep_fn); rc != 0) {
        log_error("Failed to delete I/O completion queue id {} controller {} error {}", cq_id, *ctrlr, rc);
        return std::unexpected(rc);
    }
    spdk_nvme_ctrlr_free_qid(ctrlr, cq_id);

    return { };
}

std::expected<NvmeBackend::QueueInfo, int> SpdkBackend::create_sq(
    void* ctrlr_handle, uint64_t iova, uint32_t queue_size, uint32_t cq_id, const SleepFn& sleep_fn)
{
    auto* ctrlr = static_cast<spdk_nvme_ctrlr*>(ctrlr_handle);
    if (queue_size < 2) {
        log_warn("Invalid submission queue size {}; minimum entries 2", queue_size);
        return std::unexpected(EINVAL);
    }

    auto sq_id = static_cast<uint16_t>(cq_id);

    struct spdk_nvme_cmd cmd { };
    cmd.opc = SPDK_NVME_OPC_CREATE_IO_SQ;
    cmd.cdw10_bits.create_io_q.qid = sq_id;
    cmd.cdw10_bits.create_io_q.qsize = queue_size - 1; // 0 based.
    cmd.cdw11_bits.create_io_sq.pc = 1;
    cmd.cdw11_bits.create_io_sq.cqid = cq_id;
    cmd.dptr.prp.prp1 = iova;

    if (auto rc = execute_admin_command(ctrlr_handle, cmd, sleep_fn); rc != 0) {
        log_error("Failed to create I/O submission queue id {} controller {} error {}", sq_id, *ctrlr, rc);
        return std::unexpected(rc);
    }

    auto dstrd = spdk_nvme_ctrlr_get_registers(ctrlr)->cap.bits.dstrd;
    auto db_offset = 0x1000 + (2 * sq_id) * (4u << dstrd);
    return QueueInfo { sq_id, db_offset };
}

std::expected<void, int> SpdkBackend::delete_sq(void* ctrlr_handle, uint32_t sq_id, const SleepFn& sleep_fn)
{
    auto* ctrlr = static_cast<spdk_nvme_ctrlr*>(ctrlr_handle);

    struct spdk_nvme_cmd cmd { };
    cmd.opc = SPDK_NVME_OPC_DELETE_IO_SQ;
    cmd.cdw10_bits.delete_io_q.qid = sq_id;

    if (auto rc = execute_admin_command(ctrlr_handle, cmd, sleep_fn); rc != 0) {
        log_error("Failed to delete I/O submission queue id {} controller {} error {}", sq_id, *ctrlr, rc);
        return std::unexpected(rc);
    }

    return { };
}

int SpdkBackend::execute_admin_command(void* ctrlr_handle, spdk_nvme_cmd& cmd, const SleepFn& sleep_fn) const
{
    const auto timeout = admin_command_timeouts_.find(ctrlr_handle);
    if (timeout == admin_command_timeouts_.end()) {
        log_error("No admin command timeout registered for controller {}", ctrlr_handle);
        return ENODEV;
    }
    return spdk_backend_detail::do_admin_cmd_sync(
        static_cast<spdk_nvme_ctrlr*>(ctrlr_handle), cmd, sleep_fn, timeout->second);
}
