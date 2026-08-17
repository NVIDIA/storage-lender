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

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

TEST(FramingTest, RoundTripsZeroLength)
{
    storage_lender::wire::FrameHeader header { };
    ASSERT_TRUE(storage_lender::wire::encode_frame_header(0, header));

    uint32_t length = 1;
    ASSERT_TRUE(storage_lender::wire::decode_frame_header(header, length));
    EXPECT_EQ(length, 0U);
}

TEST(FramingTest, RoundTripsMaximumLength)
{
    storage_lender::wire::FrameHeader header { };
    ASSERT_TRUE(storage_lender::wire::encode_frame_header(storage_lender::wire::MAX_FRAME_BYTES, header));

    uint32_t length = 0;
    ASSERT_TRUE(storage_lender::wire::decode_frame_header(header, length));
    EXPECT_EQ(length, storage_lender::wire::MAX_FRAME_BYTES);
}

TEST(FramingTest, RejectsOversizedLength)
{
    storage_lender::wire::FrameHeader header { };

    EXPECT_FALSE(storage_lender::wire::encode_frame_header(
        static_cast<std::size_t>(storage_lender::wire::MAX_FRAME_BYTES) + 1, header));
}
