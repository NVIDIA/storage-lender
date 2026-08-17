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

#include <array>
#include <cstddef>
#include <cstdint>

namespace storage_lender::wire {

constexpr uint32_t MAX_FRAME_BYTES = 64U * 1024;

using FrameHeader = std::array<std::byte, sizeof(uint32_t)>;

bool encode_frame_header(std::size_t payload_size, FrameHeader& header);
bool decode_frame_header(const FrameHeader& header, uint32_t& payload_size);

} // namespace storage_lender::wire
