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

#include "detail/posix_raii.hpp"

#include <cstddef>
#include <expected>

using UniqueFd = storage_lender::server_detail::UniqueFd;

std::size_t system_page_size();
std::expected<UniqueFd, int> make_sealed_memfd(std::size_t size);
std::expected<UniqueFd, int> make_unsealed_memfd(std::size_t size);
