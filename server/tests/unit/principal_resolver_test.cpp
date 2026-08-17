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

#include "principal_resolver.hpp"

#include <gtest/gtest.h>

TEST(PrincipalResolverTest, UidMappingWinsOverConflictingGidMapping)
{
    PrincipalResolver resolver {
        { { 1001, "camera" } },
        { { 2001, "storage" } },
    };

    EXPECT_EQ(resolver.resolve(1001, 2001), "camera");
}

TEST(PrincipalResolverTest, GidMappingIsUsedWhenUidIsUnmatched)
{
    PrincipalResolver resolver { { }, { { 2001, "storage" } } };
    EXPECT_EQ(resolver.resolve(1001, 2001), "storage");
}

TEST(PrincipalResolverTest, UnmatchedPeerUsesDefaultPrincipal)
{
    PrincipalResolver resolver { { }, { } };
    EXPECT_EQ(resolver.resolve(1001, 2001), "default");
}
