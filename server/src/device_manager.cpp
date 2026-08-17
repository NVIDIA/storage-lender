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

#include "device_manager.hpp"

#include "pci_address.hpp"

#include <algorithm>
#include <utility>

DeviceManager::DeviceManager(NvmeBackend& backend, DevicePolicy policy)
    : backend_ { backend }
    , device_policy_ { std::move(policy) }
{
}

DeviceManager::~DeviceManager() { close_all(); }

std::expected<uint32_t, LenderError> DeviceManager::open_device(const std::string& pci_address, OpenDeviceMode mode)
{
    const auto canonical = canonicalize_pci_bdf(pci_address);
    if (!canonical) {
        return std::unexpected(LenderError::INVALID_ARGUMENT);
    }

    auto controller_it = controllers_.find(*canonical);
    if (controller_it != controllers_.end()) {
        if (controller_it->second.mode == OpenDeviceMode::EXCLUSIVE || mode == OpenDeviceMode::EXCLUSIVE) {
            return std::unexpected(LenderError::PERMISSION_DENIED);
        }
        ++controller_it->second.ref_count;
    } else {
        auto connect_r = backend_.nvme_connect(*canonical, device_policy_.options_for(*canonical));
        if (!connect_r) {
            return std::unexpected(LenderError::NOT_FOUND);
        }
        controllers_.emplace(*canonical, ControllerEntry { *connect_r, mode, 1 });
    }

    const auto device_id = next_device_id_++;
    device_handles_.emplace(device_id, *canonical);
    return device_id;
}

std::expected<void, LenderError> DeviceManager::close_device(uint32_t device_id)
{
    auto handle_it = device_handles_.find(device_id);
    if (handle_it == device_handles_.end()) {
        return std::unexpected(LenderError::NOT_FOUND);
    }

    auto controller_it = controllers_.find(handle_it->second);
    if (controller_it == controllers_.end()) {
        return std::unexpected(LenderError::INTERNAL);
    }

    if (controller_it->second.ref_count == 1) {
        auto detached = backend_.nvme_detach(controller_it->second.ctrlr);
        if (!detached) {
            return std::unexpected(LenderError::INTERNAL);
        }
        device_handles_.erase(handle_it);
        controllers_.erase(controller_it);
        return { };
    }

    --controller_it->second.ref_count;
    device_handles_.erase(handle_it);
    return { };
}

std::expected<NvmeBackend::ControllerInfo, LenderError> DeviceManager::get_controller_info(uint32_t device_id) const
{
    auto ctrlr = get_ctrlr(device_id);
    if (!ctrlr) {
        return std::unexpected(ctrlr.error());
    }
    return backend_.get_controller_info(*ctrlr).transform_error([](int) { return LenderError::INTERNAL; });
}

std::expected<void*, LenderError> DeviceManager::get_ctrlr(uint32_t device_id) const
{
    auto handle_it = device_handles_.find(device_id);
    if (handle_it == device_handles_.end()) {
        return std::unexpected(LenderError::NOT_FOUND);
    }

    auto controller_it = controllers_.find(handle_it->second);
    if (controller_it == controllers_.end()) {
        return std::unexpected(LenderError::INTERNAL);
    }
    return controller_it->second.ctrlr;
}

void DeviceManager::close_all()
{
    std::ranges::for_each(controllers_, [this](auto& entry) { backend_.nvme_detach(entry.second.ctrlr); });
    device_handles_.clear();
    controllers_.clear();
}
