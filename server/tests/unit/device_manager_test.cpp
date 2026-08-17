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

#include "device_manager.hpp"
#include "device_policy.hpp"
#include "mock_nvme_backend.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>

using ::testing::_;
using ::testing::Return;

class DeviceManagerTest : public ::testing::Test {
protected:
    ::testing::NiceMock<MockNvmeBackend> mock_;
    DevicePolicy policy_ {
        .default_options = {
            .num_io_queues = 65534,
            .admin_command_timeout = std::chrono::milliseconds { 10000 },
        },
        .overrides = {
            { "0000:00:02.0",
                {
                    .num_io_queues = 2048,
                    .admin_command_timeout = std::chrono::milliseconds { 30000 },
                } },
        },
    };
    DeviceManager dm_ { mock_, policy_ };
    void* const CTRLR = reinterpret_cast<void*>(0x1234);
};

TEST_F(DeviceManagerTest, OpenDeviceCanonicalizesBdfAndPassesResolvedOptions)
{
    const NvmeConnectOptions expected {
        .num_io_queues = 2048,
        .admin_command_timeout = std::chrono::milliseconds { 30000 },
    };
    EXPECT_CALL(mock_, nvme_connect("0000:00:02.0", expected)).WillOnce(Return(CTRLR));
    EXPECT_TRUE(dm_.open_device("00:02.0", OpenDeviceMode::SHARED));
}

TEST_F(DeviceManagerTest, EquivalentBdfSpellingsShareOneController)
{
    EXPECT_CALL(mock_, nvme_connect("0000:0a:1f.7", _)).WillOnce(Return(CTRLR));
    EXPECT_TRUE(dm_.open_device("0A:1F.7", OpenDeviceMode::SHARED));
    EXPECT_TRUE(dm_.open_device("0000:0a:1f.7", OpenDeviceMode::SHARED));
}

TEST_F(DeviceManagerTest, EquivalentBdfSpellingCannotBypassExclusiveOpen)
{
    EXPECT_CALL(mock_, nvme_connect("0000:0a:1f.7", _)).WillOnce(Return(CTRLR));
    EXPECT_TRUE(dm_.open_device("0A:1F.7", OpenDeviceMode::SHARED));
    EXPECT_EQ(
        dm_.open_device("0000:0a:1f.7", OpenDeviceMode::EXCLUSIVE), std::unexpected(LenderError::PERMISSION_DENIED));
}

TEST_F(DeviceManagerTest, OpenDeviceReturnsDeviceId)
{
    EXPECT_CALL(mock_, nvme_connect("0000:00:01.0", _)).WillOnce(Return(CTRLR));

    auto result = dm_.open_device("0000:00:01.0", OpenDeviceMode::SHARED);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1u);
}

TEST_F(DeviceManagerTest, OpenDeviceAcceptsDomainlessBdf)
{
    EXPECT_CALL(mock_, nvme_connect("0000:00:01.0", _)).WillOnce(Return(CTRLR));

    auto result = dm_.open_device("00:01.0", OpenDeviceMode::SHARED);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1u);
}

TEST_F(DeviceManagerTest, OpenDeviceRejectsMalformedBdf)
{
    EXPECT_CALL(mock_, nvme_connect(_, _)).Times(0);

    auto result = dm_.open_device("0000:00:01.0 trsvcid:4420", OpenDeviceMode::SHARED);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::INVALID_ARGUMENT);
}

TEST_F(DeviceManagerTest, OpenDeviceNotFoundOnNullCtrlr)
{
    EXPECT_CALL(mock_, nvme_connect(_, _)).WillOnce(Return(std::unexpected(-1)));

    auto result = dm_.open_device("0000:00:02.0", OpenDeviceMode::SHARED);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::NOT_FOUND);
}

TEST_F(DeviceManagerTest, SharedOpenReturnsUniqueDeviceIds)
{
    EXPECT_CALL(mock_, nvme_connect("0000:00:01.0", _)).WillOnce(Return(CTRLR));

    auto id1 = dm_.open_device("0000:00:01.0", OpenDeviceMode::SHARED);
    auto id2 = dm_.open_device("0000:00:01.0", OpenDeviceMode::SHARED);
    ASSERT_TRUE(id1.has_value());
    ASSERT_TRUE(id2.has_value());
    EXPECT_NE(*id1, *id2);
}

TEST_F(DeviceManagerTest, ExclusiveOpenDeniedWhenSharedExists)
{
    EXPECT_CALL(mock_, nvme_connect("0000:00:01.0", _)).WillOnce(Return(CTRLR));

    dm_.open_device("0000:00:01.0", OpenDeviceMode::SHARED);
    auto result = dm_.open_device("0000:00:01.0", OpenDeviceMode::EXCLUSIVE);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::PERMISSION_DENIED);
}

TEST_F(DeviceManagerTest, SharedOpenDeniedWhenExclusiveExists)
{
    EXPECT_CALL(mock_, nvme_connect("0000:00:01.0", _)).WillOnce(Return(CTRLR));

    dm_.open_device("0000:00:01.0", OpenDeviceMode::EXCLUSIVE);
    auto result = dm_.open_device("0000:00:01.0", OpenDeviceMode::SHARED);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::PERMISSION_DENIED);
}

TEST_F(DeviceManagerTest, CloseDeviceCallsDetachAtZeroRef)
{
    EXPECT_CALL(mock_, nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(mock_, nvme_detach(CTRLR)).Times(1);

    auto id = dm_.open_device("0000:00:01.0", OpenDeviceMode::EXCLUSIVE);
    ASSERT_TRUE(id.has_value());
    auto result = dm_.close_device(*id);
    ASSERT_TRUE(result.has_value());
}

TEST_F(DeviceManagerTest, CloseDeviceDoesNotDetachBeforeZeroRef)
{
    EXPECT_CALL(mock_, nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(mock_, nvme_detach(_)).Times(0);

    auto id1 = dm_.open_device("0000:00:01.0", OpenDeviceMode::SHARED);
    auto id2 = dm_.open_device("0000:00:01.0", OpenDeviceMode::SHARED);
    ASSERT_TRUE(id1.has_value());
    ASSERT_TRUE(id2.has_value());
    auto result = dm_.close_device(*id1);
    ASSERT_TRUE(result.has_value());

    ::testing::Mock::VerifyAndClearExpectations(&mock_);
    EXPECT_CALL(mock_, nvme_detach(CTRLR)).Times(1);
    EXPECT_TRUE(dm_.close_device(*id2).has_value());
}

TEST_F(DeviceManagerTest, FailedFinalDetachRetainsHandleForRetry)
{
    EXPECT_CALL(mock_, nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(mock_, nvme_detach(CTRLR))
        .WillOnce(Return(std::unexpected(-1)))
        .WillOnce(Return(std::expected<void, int> { }));

    auto id = dm_.open_device("0000:00:01.0", OpenDeviceMode::EXCLUSIVE);
    ASSERT_TRUE(id);
    EXPECT_EQ(dm_.close_device(*id), std::unexpected(LenderError::INTERNAL));
    EXPECT_EQ(*dm_.get_ctrlr(*id), CTRLR);
    EXPECT_TRUE(dm_.close_device(*id));
}

TEST_F(DeviceManagerTest, ClosedDeviceIdCannotBeReused)
{
    EXPECT_CALL(mock_, nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(mock_, nvme_detach(CTRLR)).Times(1);

    auto id1 = dm_.open_device("0000:00:01.0", OpenDeviceMode::SHARED);
    auto id2 = dm_.open_device("0000:00:01.0", OpenDeviceMode::SHARED);
    ASSERT_TRUE(id1.has_value());
    ASSERT_TRUE(id2.has_value());

    EXPECT_TRUE(dm_.close_device(*id1).has_value());
    auto repeated_close = dm_.close_device(*id1);
    ASSERT_FALSE(repeated_close.has_value());
    EXPECT_EQ(repeated_close.error(), LenderError::NOT_FOUND);

    auto ctrlr = dm_.get_ctrlr(*id2);
    ASSERT_TRUE(ctrlr.has_value());
    EXPECT_EQ(*ctrlr, CTRLR);
    EXPECT_TRUE(dm_.close_device(*id2).has_value());
}

TEST_F(DeviceManagerTest, CloseDeviceNotFoundOnBadId)
{
    auto result = dm_.close_device(99);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::NOT_FOUND);
}

TEST_F(DeviceManagerTest, GetDeviceInfoDelegatesToBackend)
{
    NvmeBackend::ControllerInfo info;
    info.model = "TestDrive";
    info.pci_resource_path = "/sys/bus/pci/devices/0000:00:01.0/resource0";
    info.max_queue_entries = 1024;
    info.page_size = 4096;
    info.num_io_queues = 4;

    EXPECT_CALL(mock_, nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(mock_, get_controller_info(CTRLR)).WillOnce(Return(info));

    auto id = dm_.open_device("0000:00:01.0", OpenDeviceMode::SHARED);
    ASSERT_TRUE(id.has_value());

    auto result = dm_.get_controller_info(*id);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->model, "TestDrive");
    EXPECT_EQ(result->pci_resource_path, "/sys/bus/pci/devices/0000:00:01.0/resource0");
    EXPECT_EQ(result->max_queue_entries, 1024u);
}

TEST_F(DeviceManagerTest, GetCtrlrReturnsHandle)
{
    EXPECT_CALL(mock_, nvme_connect(_, _)).WillOnce(Return(CTRLR));

    auto id = dm_.open_device("0000:00:01.0", OpenDeviceMode::SHARED);
    ASSERT_TRUE(id.has_value());

    auto ctrlr = dm_.get_ctrlr(*id);
    ASSERT_TRUE(ctrlr.has_value());
    EXPECT_EQ(*ctrlr, CTRLR);
}

TEST_F(DeviceManagerTest, GetCtrlrNotFoundOnBadId)
{
    auto result = dm_.get_ctrlr(99);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::NOT_FOUND);
}

TEST_F(DeviceManagerTest, GetDeviceInfoNotFoundOnBadId)
{
    auto result = dm_.get_controller_info(99);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::NOT_FOUND);
}

TEST_F(DeviceManagerTest, GetDeviceInfoBackendFails)
{
    EXPECT_CALL(mock_, nvme_connect(_, _)).WillOnce(Return(CTRLR));
    EXPECT_CALL(mock_, get_controller_info(CTRLR)).WillOnce(Return(std::unexpected(-1)));

    auto id = dm_.open_device("0000:00:01.0", OpenDeviceMode::SHARED);
    ASSERT_TRUE(id.has_value());

    auto result = dm_.get_controller_info(*id);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LenderError::INTERNAL);
}
