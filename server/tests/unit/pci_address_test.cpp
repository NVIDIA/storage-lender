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

#include "pci_address.hpp"

#include <gtest/gtest.h>

TEST(PciAddressTest, CanonicalizesDomainCaseAndDomainlessForms)
{
    EXPECT_EQ(canonicalize_pci_bdf("0A:1F.7"), std::optional<std::string> { "0000:0a:1f.7" });
    EXPECT_EQ(canonicalize_pci_bdf("0000:0a:1f.7"), std::optional<std::string> { "0000:0a:1f.7" });
    EXPECT_EQ(canonicalize_pci_bdf("ABCD:0A:1F.7"), std::optional<std::string> { "abcd:0a:1f.7" });
}

TEST(PciAddressTest, CanonicalizationRejectsMalformedAddresses)
{
    EXPECT_EQ(canonicalize_pci_bdf("0000:0g:1f.0"), std::nullopt);
    EXPECT_EQ(canonicalize_pci_bdf("0a:1f.8"), std::nullopt);
    EXPECT_EQ(canonicalize_pci_bdf("0000:0a:1f.0 trailing"), std::nullopt);
}

TEST(PciAddressTest, AcceptsHexCaseInsensitively)
{
    EXPECT_TRUE(is_valid_pci_bdf("0000:0a:1f.7"));
    EXPECT_TRUE(is_valid_pci_bdf("ABCD:0A:1F.7"));
    EXPECT_TRUE(is_valid_pci_bdf("0A:1F.7"));
}

TEST(PciAddressTest, RejectsNonHexCharacters)
{
    EXPECT_FALSE(is_valid_pci_bdf("000g:0a:1f.0"));
    EXPECT_FALSE(is_valid_pci_bdf("0000:0g:1f.0"));
    EXPECT_FALSE(is_valid_pci_bdf("0000:0a:1g.0"));
    EXPECT_FALSE(is_valid_pci_bdf("0g:1f.0"));
}
