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

#include <boost/system/error_code.hpp>

// How the accept loop should react to an async_accept error.
enum class AcceptErrorAction {
    Stop, // Intended shutdown (acceptor closed/cancelled).
    Continue, // Per-connection transient error; retry immediately.
    Backoff, // Resource exhaustion; retry after a short delay to avoid a busy-spin.
};

AcceptErrorAction classify_accept_error(const boost::system::error_code& ec);
