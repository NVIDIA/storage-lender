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

#include "logging.hpp"

#include <gtest/gtest.h>

class LoggingTest : public ::testing::Test {
protected:
    void SetUp() override { original_level_ = logging::active_level(); }
    void TearDown() override { logging::set_level(original_level_); }

    LogLevel original_level_ { LogLevel::INFO };
};

TEST(LoggingPolicyTest, BuildConfigurationDefinesCompiledFloorAndDefault)
{
#if SPDLOG_ACTIVE_LEVEL == SPDLOG_LEVEL_TRACE
    EXPECT_EQ(logging::compiled_minimum_level(), LogLevel::TRACE);
    EXPECT_EQ(logging::default_level(), LogLevel::TRACE);
#elif SPDLOG_ACTIVE_LEVEL == SPDLOG_LEVEL_DEBUG
    EXPECT_EQ(logging::compiled_minimum_level(), LogLevel::DEBUG);
    EXPECT_EQ(logging::default_level(), LogLevel::INFO);
#else
    FAIL() << "unsupported SPDLOG_ACTIVE_LEVEL";
#endif
}

TEST(LoggingPolicyTest, AvailabilityFollowsCompiledFloor)
{
    EXPECT_EQ(logging::is_available(LogLevel::TRACE), logging::compiled_minimum_level() == LogLevel::TRACE);
    EXPECT_TRUE(logging::is_available(LogLevel::DEBUG));
    EXPECT_TRUE(logging::is_available(LogLevel::INFO));
    EXPECT_TRUE(logging::is_available(LogLevel::OFF));
}

TEST(LoggingPolicyTest, NamesEveryConfiguredLevel)
{
    EXPECT_EQ(log_level_name(LogLevel::TRACE), "trace");
    EXPECT_EQ(log_level_name(LogLevel::DEBUG), "debug");
    EXPECT_EQ(log_level_name(LogLevel::INFO), "info");
    EXPECT_EQ(log_level_name(LogLevel::WARN), "warn");
    EXPECT_EQ(log_level_name(LogLevel::ERROR), "error");
    EXPECT_EQ(log_level_name(LogLevel::CRITICAL), "critical");
    EXPECT_EQ(log_level_name(LogLevel::OFF), "off");
}

TEST(LoggingPolicyTest, ThresholdPredicateMatchesSeverityOrder)
{
    EXPECT_TRUE(log_level_enabled(LogLevel::INFO, LogLevel::INFO));
    EXPECT_TRUE(log_level_enabled(LogLevel::INFO, LogLevel::ERROR));
    EXPECT_FALSE(log_level_enabled(LogLevel::INFO, LogLevel::DEBUG));
    EXPECT_FALSE(log_level_enabled(LogLevel::OFF, LogLevel::CRITICAL));
}

TEST_F(LoggingTest, RuntimeLevelCanChangeAmongCompiledLevels)
{
    logging::set_level(LogLevel::ERROR);
    EXPECT_EQ(logging::active_level(), LogLevel::ERROR);

    logging::set_level(LogLevel::DEBUG);
    EXPECT_EQ(logging::active_level(), LogLevel::DEBUG);
}

TEST_F(LoggingTest, InitializeInstallsBuildDefault)
{
    logging::set_level(LogLevel::ERROR);
    logging::initialize();
    EXPECT_EQ(logging::active_level(), logging::default_level());
}
