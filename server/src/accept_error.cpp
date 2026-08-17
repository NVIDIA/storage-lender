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

#include "accept_error.hpp"

#include <boost/asio/error.hpp>
#include <boost/system/errc.hpp>

AcceptErrorAction classify_accept_error(const boost::system::error_code& ec)
{
    namespace errc = boost::system::errc;

    if (ec == boost::asio::error::operation_aborted || ec == boost::asio::error::bad_descriptor) {
        return AcceptErrorAction::Stop; // acceptor cancelled or closed — nothing left to accept
    }
    if (ec == errc::too_many_files_open || ec == errc::too_many_files_open_in_system || ec == errc::no_buffer_space
        || ec == errc::not_enough_memory) {
        return AcceptErrorAction::Backoff;
    }
    // Default: retry immediately. The reachable non-Stop/Backoff accept errors on this
    // AF_UNIX listener (ECONNABORTED, EINTR, EPROTO) are self-limiting — each drains one
    // failed pending connection, then accept returns EAGAIN. A hypothetical persistent,
    // non-draining error would busy-loop here; bounding that (consecutive-error backoff
    // escalation) is a deferred fast-follow, not a default -> Backoff flip (which would
    // throttle legitimate connection churn).
    return AcceptErrorAction::Continue;
}
