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

#include "device_policy.hpp"
#include "lender_error.hpp"
#include "nvme_backend.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <unordered_map>
#include <utility>

enum class OpenDeviceMode { SHARED = 0, EXCLUSIVE = 1 };

inline auto format_as(OpenDeviceMode mode)
{
    switch (mode) {
    case OpenDeviceMode::SHARED:
        return std::string("SHARED");
    case OpenDeviceMode::EXCLUSIVE:
        return std::string("EXCLUSIVE");
    }
    return std::format("(unrecognized open device mode {})", std::to_underlying(mode));
}

struct ControllerEntry {
    void* ctrlr;
    OpenDeviceMode mode;
    uint32_t ref_count;
};

class DeviceManager {
public:
    DeviceManager(NvmeBackend& backend, DevicePolicy policy);
    ~DeviceManager();

    std::expected<uint32_t, LenderError> open_device(const std::string& pci_address, OpenDeviceMode mode);

    std::expected<void, LenderError> close_device(uint32_t device_id);

    std::expected<NvmeBackend::ControllerInfo, LenderError> get_controller_info(uint32_t device_id) const;

    std::expected<void*, LenderError> get_ctrlr(uint32_t device_id) const;

    void close_all();

private:
    NvmeBackend& backend_;
    DevicePolicy device_policy_;

    uint32_t next_device_id_ { 1 };
    std::unordered_map<uint32_t, std::string> device_handles_;
    std::unordered_map<std::string, ControllerEntry> controllers_;
};
