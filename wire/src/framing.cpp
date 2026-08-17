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

#include "storage_lender/wire/framing.hpp"

#include <cstring>
#include <endian.h>

namespace storage_lender::wire {

bool encode_frame_header(std::size_t payload_size, FrameHeader& header)
{
    if (payload_size > MAX_FRAME_BYTES) {
        return false;
    }

    const auto little_endian_length = htole32(static_cast<uint32_t>(payload_size));
    std::memcpy(header.data(), &little_endian_length, sizeof(little_endian_length));
    return true;
}

bool decode_frame_header(const FrameHeader& header, uint32_t& payload_size)
{
    uint32_t little_endian_length = 0;
    std::memcpy(&little_endian_length, header.data(), sizeof(little_endian_length));

    payload_size = le32toh(little_endian_length);
    return payload_size <= MAX_FRAME_BYTES;
}

} // namespace storage_lender::wire
