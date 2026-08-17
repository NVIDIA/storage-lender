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

#include <utility>

PrincipalResolver::PrincipalResolver(UidMappings uids, GidMappings gids)
    : uids_(std::move(uids))
    , gids_(std::move(gids))
{
}

PrincipalId PrincipalResolver::resolve(uid_t uid, gid_t gid) const
{
    auto uid_it = uids_.find(uid);
    if (uid_it != uids_.end()) {
        return uid_it->second;
    }

    auto gid_it = gids_.find(gid);
    if (gid_it != gids_.end()) {
        return gid_it->second;
    }

    return "default";
}
