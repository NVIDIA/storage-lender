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

#include <algorithm>
#include <regex>

std::optional<std::string> canonicalize_pci_bdf(std::string_view pci_address)
{
    static const std::regex BDF_RE {
        R"(^([0-9a-f]{4}:)?[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]$)",
        std::regex_constants::icase,
    };
    if (!std::regex_match(pci_address.begin(), pci_address.end(), BDF_RE)) {
        return std::nullopt;
    }

    auto canonical = pci_address.size() == 7 ? "0000:" + std::string { pci_address } : std::string { pci_address };
    std::ranges::transform(canonical, canonical.begin(), [](char character) {
        if (character >= 'A' && character <= 'F') {
            return static_cast<char>(character - 'A' + 'a');
        }
        return character;
    });
    return canonical;
}

bool is_valid_pci_bdf(std::string_view pci_address) { return canonicalize_pci_bdf(pci_address).has_value(); }
