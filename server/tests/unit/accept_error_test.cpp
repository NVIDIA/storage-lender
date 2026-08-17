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

#include <gtest/gtest.h>

#include <cerrno>

namespace {

boost::system::error_code system_ec(int errno_value)
{
    return boost::system::error_code(errno_value, boost::system::system_category());
}

} // namespace

TEST(AcceptErrorClassifier, ShutdownStops)
{
    EXPECT_EQ(classify_accept_error(boost::asio::error::operation_aborted), AcceptErrorAction::Stop);
    EXPECT_EQ(classify_accept_error(boost::asio::error::bad_descriptor), AcceptErrorAction::Stop);
}

TEST(AcceptErrorClassifier, ResourceExhaustionBacksOff)
{
    for (const int e : { EMFILE, ENFILE, ENOBUFS, ENOMEM }) {
        EXPECT_EQ(classify_accept_error(system_ec(e)), AcceptErrorAction::Backoff) << "errno=" << e;
    }
}

TEST(AcceptErrorClassifier, TransientErrorsContinue)
{
    for (const int e : { ECONNABORTED, EINTR, EPROTO }) {
        EXPECT_EQ(classify_accept_error(system_ec(e)), AcceptErrorAction::Continue) << "errno=" << e;
    }
}
