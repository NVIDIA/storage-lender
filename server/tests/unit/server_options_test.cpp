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

#include "server_options.hpp"

#include <gtest/gtest.h>

#include <array>

TEST(ServerOptionsTest, NoArgumentsUsesEtcDefault)
{
    const auto options = parse_server_options({ });
    ASSERT_TRUE(options);
    EXPECT_EQ(options->config_path, "/etc/storage-lender/config.toml");
}

TEST(ServerOptionsTest, ConfigFlagSelectsExplicitPath)
{
    const std::array<std::string_view, 2> arguments { "--config", "/chosen/config.toml" };
    const auto options = parse_server_options(arguments);
    ASSERT_TRUE(options);
    EXPECT_EQ(options->config_path, "/chosen/config.toml");
}

TEST(ServerOptionsTest, MissingConfigValueIsRejected)
{
    const std::array<std::string_view, 1> arguments { "--config" };
    EXPECT_FALSE(parse_server_options(arguments));
}

TEST(ServerOptionsTest, UnknownOrRepeatedArgumentsAreRejected)
{
    const std::array<std::string_view, 1> unknown { "--unknown" };
    EXPECT_FALSE(parse_server_options(unknown));

    const std::array<std::string_view, 4> repeated { "--config", "one", "--config", "two" };
    EXPECT_FALSE(parse_server_options(repeated));
}
