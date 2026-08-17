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

#include "storage_lender/types.hpp"

#include <string>

namespace storage_lender {

class StorageLenderClient {
public:
    static ClientError connect(const std::string& socket_path, StorageLenderClient& out);

    StorageLenderClient() noexcept;
    StorageLenderClient(StorageLenderClient&&) noexcept;
    StorageLenderClient& operator=(StorageLenderClient&&) noexcept;
    ~StorageLenderClient();

    StorageLenderClient(const StorageLenderClient&) = delete;
    StorageLenderClient& operator=(const StorageLenderClient&) = delete;

    ClientError transfer_fd(int fd, uint32_t& fd_id);

    ClientError map_buffer(uint32_t fd_id, uint64_t size, uint64_t& iova, uint32_t alignment = 0);

    ClientError unmap_buffer(uint64_t iova);

    ClientError open_device(const std::string& pci_address, OpenDeviceMode mode, uint32_t& device_id);

    ClientError close_device(uint32_t device_id);

    ClientError get_device_info(uint32_t device_id, DeviceInfo& info);

    ClientError create_cq(uint32_t device_id, uint64_t iova, uint32_t queue_size, QueueInfo& info);

    ClientError delete_cq(uint32_t cq_id);

    ClientError create_sq(uint32_t device_id, uint32_t cq_id, uint64_t iova, uint32_t queue_size, QueueInfo& info);

    ClientError delete_sq(uint32_t sq_id);

private:
    explicit StorageLenderClient(int sock_fd);

    int sock_fd_;

    bool write_exact(const void* buf, size_t n);
    bool read_exact(void* buf, size_t n);
    bool send_frame(const std::string& data);
    bool recv_frame(std::string& data);

    ClientError rpc(
        uint32_t method, int32_t& status_code, std::string& response_payload, const std::string& request = { });
};

} // namespace storage_lender
