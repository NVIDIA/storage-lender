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

#include "options.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string_view>

namespace {

using storage_lender::ctl::ColorMode;
using storage_lender::ctl::CtlCommand;
using storage_lender::ctl::DEFAULT_CTL_SOCKET;
using storage_lender::ctl::OutputFormat;
using storage_lender::ctl::parse_ctl_options;

TEST(CtlOptionsTest, UsesDefaultsForQuota)
{
    constexpr std::array ARGUMENTS { std::string_view { "quota" } };

    auto options = parse_ctl_options(ARGUMENTS);

    ASSERT_TRUE(options.has_value());
    EXPECT_EQ(options->socket_path, DEFAULT_CTL_SOCKET);
    EXPECT_EQ(options->format, OutputFormat::HUMAN);
    EXPECT_EQ(options->color, ColorMode::AUTO);
    EXPECT_EQ(options->command, CtlCommand::QUOTA);
    EXPECT_FALSE(options->help);
    EXPECT_FALSE(options->version);
}

TEST(CtlOptionsTest, SelectsQuotaOrLatency)
{
    constexpr std::array QUOTA_ARGUMENTS { std::string_view { "quota" } };
    auto quota = parse_ctl_options(QUOTA_ARGUMENTS);
    ASSERT_TRUE(quota.has_value());
    EXPECT_EQ(quota->command, CtlCommand::QUOTA);

    constexpr std::array LATENCY_ARGUMENTS { std::string_view { "latency" } };
    auto latency = parse_ctl_options(LATENCY_ARGUMENTS);
    ASSERT_TRUE(latency.has_value());
    EXPECT_EQ(latency->command, CtlCommand::LATENCY);
}

TEST(CtlOptionsTest, AcceptsOptionsBeforeOrAfterLatency)
{
    constexpr std::array BEFORE_LATENCY {
        std::string_view { "--format" },
        std::string_view { "json" },
        std::string_view { "latency" },
    };
    auto before = parse_ctl_options(BEFORE_LATENCY);
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->command, CtlCommand::LATENCY);
    EXPECT_EQ(before->format, OutputFormat::JSON);

    constexpr std::array AFTER_LATENCY {
        std::string_view { "latency" },
        std::string_view { "--socket" },
        std::string_view { "/run/custom.sock" },
    };
    auto after = parse_ctl_options(AFTER_LATENCY);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->command, CtlCommand::LATENCY);
    EXPECT_EQ(after->socket_path, "/run/custom.sock");
}

TEST(CtlOptionsTest, RejectsMultipleSubcommands)
{
    constexpr std::array ARGUMENTS {
        std::string_view { "quota" },
        std::string_view { "latency" },
    };

    auto options = parse_ctl_options(ARGUMENTS);

    ASSERT_FALSE(options.has_value());
    EXPECT_EQ(options.error(), "multiple ctl subcommands specified");
}

TEST(CtlOptionsTest, MissingSubcommandNamesBothChoices)
{
    constexpr std::array<std::string_view, 0> ARGUMENTS { };

    auto options = parse_ctl_options(ARGUMENTS);

    ASSERT_FALSE(options.has_value());
    EXPECT_EQ(options.error(), "expected quota or latency subcommand");
}

TEST(CtlOptionsTest, AcceptsShortAndLongOptionsBeforeOrAfterQuota)
{
    constexpr std::array BEFORE_QUOTA {
        std::string_view { "-s" },
        std::string_view { "/run/custom.sock" },
        std::string_view { "--format" },
        std::string_view { "json" },
        std::string_view { "-c" },
        std::string_view { "always" },
        std::string_view { "quota" },
    };
    auto before = parse_ctl_options(BEFORE_QUOTA);
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->socket_path, "/run/custom.sock");
    EXPECT_EQ(before->format, OutputFormat::JSON);
    EXPECT_EQ(before->color, ColorMode::ALWAYS);

    constexpr std::array AFTER_QUOTA {
        std::string_view { "quota" },
        std::string_view { "--socket" },
        std::string_view { "/run/custom.sock" },
        std::string_view { "-f" },
        std::string_view { "human" },
        std::string_view { "--color" },
        std::string_view { "never" },
    };
    auto after = parse_ctl_options(AFTER_QUOTA);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->socket_path, "/run/custom.sock");
    EXPECT_EQ(after->format, OutputFormat::HUMAN);
    EXPECT_EQ(after->color, ColorMode::NEVER);
}

TEST(CtlOptionsTest, RejectsDuplicateSocketOption)
{
    constexpr std::array ARGUMENTS {
        std::string_view { "-s" },
        std::string_view { "/run/first.sock" },
        std::string_view { "--socket" },
        std::string_view { "/run/second.sock" },
        std::string_view { "quota" },
    };

    auto options = parse_ctl_options(ARGUMENTS);

    ASSERT_FALSE(options.has_value());
    EXPECT_EQ(options.error(), "socket option specified more than once");
}

TEST(CtlOptionsTest, RejectsUnknownFormat)
{
    constexpr std::array ARGUMENTS {
        std::string_view { "quota" },
        std::string_view { "--format" },
        std::string_view { "yaml" },
    };

    auto options = parse_ctl_options(ARGUMENTS);

    ASSERT_FALSE(options.has_value());
    EXPECT_EQ(options.error(), "unknown format: yaml");
}

TEST(CtlOptionsTest, HelpAndVersionDoNotRequireQuotaOrSocket)
{
    constexpr std::array HELP_ARGUMENTS { std::string_view { "--help" } };
    auto help = parse_ctl_options(HELP_ARGUMENTS);
    ASSERT_TRUE(help.has_value());
    EXPECT_TRUE(help->help);

    constexpr std::array VERSION_ARGUMENTS { std::string_view { "-V" } };
    auto version = parse_ctl_options(VERSION_ARGUMENTS);
    ASSERT_TRUE(version.has_value());
    EXPECT_TRUE(version->version);
}

TEST(CtlOptionsTest, RejectsUnexpectedPositionalArgument)
{
    constexpr std::array ARGUMENTS {
        std::string_view { "sessions" },
    };

    auto options = parse_ctl_options(ARGUMENTS);

    ASSERT_FALSE(options.has_value());
    EXPECT_EQ(options.error(), "unexpected positional argument: sessions");
}

} // anonymous namespace
