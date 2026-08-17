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

#include "unix_socket_endpoint.hpp"

#include "detail/posix_raii.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
using storage_lender::server_detail::ScopedUmask;
using storage_lender::server_detail::UniqueFd;

struct stat directory_metadata(uid_t owner, mode_t mode)
{
    struct stat metadata { };
    metadata.st_uid = owner;
    metadata.st_mode = S_IFDIR | mode;
    return metadata;
}

std::string current_user_name()
{
    const auto* account = ::getpwuid(::geteuid());
    return account == nullptr ? std::string { } : std::string { account->pw_name };
}

std::string current_group_name()
{
    const auto* account = ::getgrgid(::getegid());
    return account == nullptr ? std::string { } : std::string { account->gr_name };
}

sockaddr_un socket_address(const std::string& path)
{
    sockaddr_un address { };
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return address;
}

int create_bound_socket(const std::string& path)
{
    const auto fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    const auto address = socket_address(path);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const auto saved_errno = errno;
        ::close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

TEST(SocketEndpointErrorTest, FormatsStructuredDiagnostic)
{
    const SocketEndpointError error { "inspect", "/runtime/api.sock", "permission denied" };

    EXPECT_EQ(error.message(), "inspect /runtime/api.sock: permission denied");
}

TEST(SocketEndpointPolicyTest, AcceptsRootOrEffectiveUserOwnedAncestors)
{
    constexpr uid_t EFFECTIVE_UID = 1234;

    EXPECT_TRUE(socket_endpoint_detail::validate_directory_metadata(
        directory_metadata(0, 0755), EFFECTIVE_UID, socket_endpoint_detail::DirectoryRole::ANCESTOR, "/root-owned")
            .has_value());
    EXPECT_TRUE(socket_endpoint_detail::validate_directory_metadata(directory_metadata(EFFECTIVE_UID, 0700),
        EFFECTIVE_UID, socket_endpoint_detail::DirectoryRole::ANCESTOR, "/user-owned")
            .has_value());
}

TEST(SocketEndpointPolicyTest, AcceptsRootOwnedStickyWritableAncestor)
{
    constexpr uid_t EFFECTIVE_UID = 1234;

    EXPECT_TRUE(socket_endpoint_detail::validate_directory_metadata(
        directory_metadata(0, 01777), EFFECTIVE_UID, socket_endpoint_detail::DirectoryRole::ANCESTOR, "/scratch")
            .has_value());
}

TEST(SocketEndpointPolicyTest, RejectsWrongOwnerWithoutStickyException)
{
    const auto result = socket_endpoint_detail::validate_directory_metadata(
        directory_metadata(2345, 0755), 1234, socket_endpoint_detail::DirectoryRole::ANCESTOR, "/wrong-owner");

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().target, "/wrong-owner");
}

TEST(SocketEndpointPolicyTest, RejectsWritableNonStickyAncestor)
{
    const auto result = socket_endpoint_detail::validate_directory_metadata(
        directory_metadata(1234, 0770), 1234, socket_endpoint_detail::DirectoryRole::ANCESTOR, "/writable");

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().target, "/writable");
}

TEST(SocketEndpointPolicyTest, RejectsWorldWritableNonStickyAncestor)
{
    const auto result = socket_endpoint_detail::validate_directory_metadata(directory_metadata(1234, 0700 | S_IWOTH),
        1234, socket_endpoint_detail::DirectoryRole::ANCESTOR, "/world-writable");

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().target, "/world-writable");
}

TEST(SocketEndpointPolicyTest, ParentMustBelongToEffectiveUser)
{
    EXPECT_TRUE(socket_endpoint_detail::validate_directory_metadata(
        directory_metadata(1234, 0700), 1234, socket_endpoint_detail::DirectoryRole::PARENT, "/safe-parent")
            .has_value());

    const auto result = socket_endpoint_detail::validate_directory_metadata(
        directory_metadata(0, 0700), 1234, socket_endpoint_detail::DirectoryRole::PARENT, "/wrong-parent");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().target, "/wrong-parent");
}

TEST(SocketEndpointPolicyTest, ParentRejectsGroupOrWorldWriteEvenWhenSticky)
{
    const auto group_writable = socket_endpoint_detail::validate_directory_metadata(
        directory_metadata(1234, 01770), 1234, socket_endpoint_detail::DirectoryRole::PARENT, "/sticky-parent");
    const auto world_writable
        = socket_endpoint_detail::validate_directory_metadata(directory_metadata(1234, 01700 | S_IWOTH), 1234,
            socket_endpoint_detail::DirectoryRole::PARENT, "/sticky-world-writable-parent");

    ASSERT_FALSE(group_writable.has_value());
    EXPECT_EQ(group_writable.error().target, "/sticky-parent");
    ASSERT_FALSE(world_writable.has_value());
    EXPECT_EQ(world_writable.error().target, "/sticky-world-writable-parent");
}

TEST(SocketEndpointPolicyTest, ParentRejectsWorldWriteWithoutSticky)
{
    const auto result = socket_endpoint_detail::validate_directory_metadata(directory_metadata(1234, 0700 | S_IWOTH),
        1234, socket_endpoint_detail::DirectoryRole::PARENT, "/world-writable-parent");

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().target, "/world-writable-parent");
}

TEST(SocketEndpointPolicyTest, RejectsNonDirectory)
{
    auto metadata = directory_metadata(1234, 0700);
    metadata.st_mode = S_IFREG | 0600;

    const auto result = socket_endpoint_detail::validate_directory_metadata(
        metadata, 1234, socket_endpoint_detail::DirectoryRole::ANCESTOR, "/not-a-directory");

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().target, "/not-a-directory");
}

class UnixSocketEndpointTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        std::error_code ec;
        const auto temporary_root = std::filesystem::temp_directory_path(ec);
        ASSERT_FALSE(ec) << ec.message();

        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            root_ = temporary_root
                / ("storage-lender-endpoint-" + std::to_string(::getpid()) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(root_, ec)) {
                owns_root_ = true;
                break;
            }
            ASSERT_EQ(ec, std::errc::file_exists) << ec.message();
            ec.clear();
        }
        ASSERT_TRUE(owns_root_);
        ASSERT_TRUE(std::filesystem::is_directory(root_, ec));
        ASSERT_FALSE(ec) << ec.message();
        ASSERT_EQ(::chmod(root_.c_str(), 0700), 0) << ::strerror(errno);

        config_ = SocketConfig { (root_ / "api.sock").string(), current_user_name(), current_group_name(), 0660 };
        ASSERT_FALSE(config_.owner.empty());
        ASSERT_FALSE(config_.group.empty());
    }

    void TearDown() override
    {
        if (!owns_root_) {
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    bool owns_root_ { false };
    std::filesystem::path root_;
    SocketConfig config_;
    boost::asio::io_context ioc_;
};

TEST_F(UnixSocketEndpointTest, CreatesListeningSocketWithExactModeDespiteUmask)
{
    ScopedUmask ambient_umask { 0777 };

    auto endpoint = UnixSocketEndpoint::create(ioc_, config_);
    ASSERT_TRUE(endpoint.has_value()) << endpoint.error().message();
    EXPECT_TRUE(endpoint->acceptor().is_open());

    struct stat metadata { };
    ASSERT_EQ(::lstat(config_.path.c_str(), &metadata), 0) << ::strerror(errno);
    EXPECT_TRUE(S_ISSOCK(metadata.st_mode));
    EXPECT_EQ(metadata.st_mode & ACCESSPERMS, config_.mode);
    EXPECT_EQ(metadata.st_uid, ::geteuid());
    EXPECT_EQ(metadata.st_gid, ::getegid());
}

TEST_F(UnixSocketEndpointTest, ClientCanConnect)
{
    auto endpoint = UnixSocketEndpoint::create(ioc_, config_);
    ASSERT_TRUE(endpoint.has_value()) << endpoint.error().message();

    UniqueFd client { ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0) };
    ASSERT_GE(client.get(), 0) << ::strerror(errno);
    const auto address = socket_address(config_.path);
    ASSERT_EQ(::connect(client.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0)
        << ::strerror(errno);
}

TEST_F(UnixSocketEndpointTest, DestructorRemovesOwnedSocket)
{
    {
        auto endpoint = UnixSocketEndpoint::create(ioc_, config_);
        ASSERT_TRUE(endpoint.has_value()) << endpoint.error().message();
        EXPECT_TRUE(std::filesystem::exists(config_.path));
    }

    EXPECT_FALSE(std::filesystem::exists(config_.path));
}

TEST_F(UnixSocketEndpointTest, RejectsMissingParent)
{
    config_.path = (root_ / "missing" / "api.sock").string();

    const auto endpoint = UnixSocketEndpoint::create(ioc_, config_);

    ASSERT_FALSE(endpoint.has_value());
    EXPECT_FALSE(std::filesystem::exists(config_.path));
}

TEST_F(UnixSocketEndpointTest, RejectsRootDirectoryAsNonRootParent)
{
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root owns the root directory";
    }
    config_.path = "/api.sock";

    const auto endpoint = UnixSocketEndpoint::create(ioc_, config_);

    ASSERT_FALSE(endpoint.has_value());
    EXPECT_EQ(endpoint.error().operation, "validate socket parent");
    EXPECT_EQ(endpoint.error().target, "/");
}

TEST_F(UnixSocketEndpointTest, RejectsWritableParent)
{
    ASSERT_EQ(::chmod(root_.c_str(), 0770), 0) << ::strerror(errno);

    const auto endpoint = UnixSocketEndpoint::create(ioc_, config_);

    ASSERT_FALSE(endpoint.has_value());
    EXPECT_EQ(endpoint.error().target, root_.string());
    EXPECT_FALSE(std::filesystem::exists(config_.path));
}

TEST_F(UnixSocketEndpointTest, RejectsSymlinkComponent)
{
    std::error_code ec;
    const auto target = root_ / "target";
    ASSERT_TRUE(std::filesystem::create_directory(target, ec)) << ec.message();
    ASSERT_EQ(::chmod(target.c_str(), 0700), 0) << ::strerror(errno);
    const auto link = root_ / "link";
    std::filesystem::create_directory_symlink(target, link, ec);
    ASSERT_FALSE(ec) << ec.message();
    config_.path = (link / "api.sock").string();

    const auto endpoint = UnixSocketEndpoint::create(ioc_, config_);

    ASSERT_FALSE(endpoint.has_value());
    EXPECT_FALSE(std::filesystem::exists(target / "api.sock"));
}

TEST_F(UnixSocketEndpointTest, RejectsUnknownOwnerBeforeCreatingEntry)
{
    config_.owner = "storage-lender-no-such-owner-" + std::to_string(::getpid());

    const auto endpoint = UnixSocketEndpoint::create(ioc_, config_);

    ASSERT_FALSE(endpoint.has_value());
    EXPECT_EQ(endpoint.error().target, config_.owner);
    EXPECT_FALSE(std::filesystem::exists(config_.path));
}

TEST_F(UnixSocketEndpointTest, RejectsUnknownGroupBeforeCreatingEntry)
{
    config_.group = "storage-lender-no-such-group-" + std::to_string(::getpid());

    const auto endpoint = UnixSocketEndpoint::create(ioc_, config_);

    ASSERT_FALSE(endpoint.has_value());
    EXPECT_EQ(endpoint.error().target, config_.group);
    EXPECT_FALSE(std::filesystem::exists(config_.path));
}

TEST_F(UnixSocketEndpointTest, RejectsKnownOwnerThatDoesNotMatchEffectiveUser)
{
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root may configure any resolved socket owner";
    }
    const auto* account = ::getpwuid(0);
    if (account == nullptr) {
        GTEST_SKIP() << "UID 0 is unavailable through NSS";
    }
    if (account->pw_uid == ::geteuid()) {
        GTEST_SKIP() << "the known NSS account matches the effective user";
    }
    config_.owner = account->pw_name;

    const auto endpoint = UnixSocketEndpoint::create(ioc_, config_);

    ASSERT_FALSE(endpoint.has_value());
    EXPECT_EQ(endpoint.error().operation, "validate socket owner");
    EXPECT_EQ(endpoint.error().target, config_.owner);
    EXPECT_FALSE(std::filesystem::exists(config_.path));
}

TEST_F(UnixSocketEndpointTest, RejectsKnownGroupOutsideEffectiveProcess)
{
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root may configure any resolved socket group";
    }
    const auto* account = ::getgrgid(0);
    if (account == nullptr) {
        GTEST_SKIP() << "GID 0 is unavailable through NSS";
    }

    const auto group_count = ::getgroups(0, nullptr);
    if (group_count < 0) {
        GTEST_SKIP() << "cannot read supplementary groups: " << ::strerror(errno);
    }
    auto groups = std::vector<gid_t>(static_cast<std::size_t>(group_count));
    if (group_count > 0 && ::getgroups(group_count, groups.data()) < 0) {
        GTEST_SKIP() << "cannot read supplementary groups: " << ::strerror(errno);
    }
    if (account->gr_gid == ::getegid() || std::find(groups.begin(), groups.end(), account->gr_gid) != groups.end()) {
        GTEST_SKIP() << "the process belongs to the known NSS group";
    }
    config_.group = account->gr_name;

    const auto endpoint = UnixSocketEndpoint::create(ioc_, config_);

    ASSERT_FALSE(endpoint.has_value());
    EXPECT_EQ(endpoint.error().operation, "validate socket group");
    EXPECT_EQ(endpoint.error().target, config_.group);
    EXPECT_FALSE(std::filesystem::exists(config_.path));
}

TEST_F(UnixSocketEndpointTest, PreservesEveryExistingEntryType)
{
    {
        const auto path = root_ / "regular.sock";
        UniqueFd regular { ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600) };
        ASSERT_GE(regular.get(), 0) << ::strerror(errno);
        config_.path = path.string();
        EXPECT_FALSE(UnixSocketEndpoint::create(ioc_, config_).has_value());
        struct stat metadata { };
        ASSERT_EQ(::lstat(path.c_str(), &metadata), 0) << ::strerror(errno);
        EXPECT_TRUE(S_ISREG(metadata.st_mode));
    }
    {
        const auto path = root_ / "directory.sock";
        std::error_code ec;
        ASSERT_TRUE(std::filesystem::create_directory(path, ec)) << ec.message();
        config_.path = path.string();
        EXPECT_FALSE(UnixSocketEndpoint::create(ioc_, config_).has_value());
        struct stat metadata { };
        ASSERT_EQ(::lstat(path.c_str(), &metadata), 0) << ::strerror(errno);
        EXPECT_TRUE(S_ISDIR(metadata.st_mode));
    }
    {
        const auto path = root_ / "symlink.sock";
        std::error_code ec;
        std::filesystem::create_symlink("missing-target", path, ec);
        ASSERT_FALSE(ec) << ec.message();
        config_.path = path.string();
        EXPECT_FALSE(UnixSocketEndpoint::create(ioc_, config_).has_value());
        struct stat metadata { };
        ASSERT_EQ(::lstat(path.c_str(), &metadata), 0) << ::strerror(errno);
        EXPECT_TRUE(S_ISLNK(metadata.st_mode));
    }
    {
        const auto path = root_ / "socket.sock";
        UniqueFd socket { create_bound_socket(path.string()) };
        ASSERT_GE(socket.get(), 0) << ::strerror(errno);
        config_.path = path.string();
        EXPECT_FALSE(UnixSocketEndpoint::create(ioc_, config_).has_value());
        struct stat metadata { };
        ASSERT_EQ(::lstat(path.c_str(), &metadata), 0) << ::strerror(errno);
        EXPECT_TRUE(S_ISSOCK(metadata.st_mode));
    }
}

TEST_F(UnixSocketEndpointTest, PreservesReplacementWithDifferentInode)
{
    ino_t original_inode { };
    ino_t replacement_inode { };
    UniqueFd replacement;
    {
        auto endpoint = UnixSocketEndpoint::create(ioc_, config_);
        ASSERT_TRUE(endpoint.has_value()) << endpoint.error().message();

        struct stat original { };
        ASSERT_EQ(::lstat(config_.path.c_str(), &original), 0) << ::strerror(errno);
        original_inode = original.st_ino;
        ASSERT_EQ(::unlink(config_.path.c_str()), 0) << ::strerror(errno);

        replacement = UniqueFd { create_bound_socket(config_.path) };
        ASSERT_GE(replacement.get(), 0) << ::strerror(errno);
        struct stat replacement_metadata { };
        ASSERT_EQ(::lstat(config_.path.c_str(), &replacement_metadata), 0) << ::strerror(errno);
        replacement_inode = replacement_metadata.st_ino;
        ASSERT_NE(replacement_inode, original_inode);
    }

    struct stat preserved { };
    ASSERT_EQ(::lstat(config_.path.c_str(), &preserved), 0) << ::strerror(errno);
    EXPECT_TRUE(S_ISSOCK(preserved.st_mode));
    EXPECT_EQ(preserved.st_ino, replacement_inode);
}

TEST_F(UnixSocketEndpointTest, CleansUpAfterFailureBeforeBoundEntryInspection)
{
    auto checkpoint = [](SocketEndpointStage stage) -> std::expected<void, SocketEndpointError> {
        if (stage == SocketEndpointStage::BOUND_UNINSPECTED) {
            return std::unexpected(SocketEndpointError { "test checkpoint", "api.sock", "injected failure" });
        }
        return { };
    };

    const auto endpoint = UnixSocketEndpoint::create_for_test(ioc_, config_, std::move(checkpoint));

    ASSERT_FALSE(endpoint.has_value());
    EXPECT_EQ(endpoint.error().operation, "test checkpoint");
    EXPECT_FALSE(std::filesystem::exists(config_.path));
}

TEST_F(UnixSocketEndpointTest, PreservesReplacementInstalledBeforeBoundEntryInspection)
{
    ino_t replacement_inode { };
    auto checkpoint = [&](SocketEndpointStage stage) -> std::expected<void, SocketEndpointError> {
        if (stage != SocketEndpointStage::BOUND_UNINSPECTED) {
            return { };
        }
        if (::unlink(config_.path.c_str()) != 0) {
            return std::unexpected(SocketEndpointError { "test setup", config_.path, "failed to remove bound socket" });
        }
        UniqueFd replacement { ::open(
            config_.path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600) };
        if (replacement.get() < 0) {
            return std::unexpected(SocketEndpointError { "test setup", config_.path, "failed to create replacement" });
        }
        struct stat metadata { };
        if (::fstat(replacement.get(), &metadata) != 0) {
            return std::unexpected(SocketEndpointError { "test setup", config_.path, "failed to inspect replacement" });
        }
        replacement_inode = metadata.st_ino;
        return { };
    };

    const auto endpoint = UnixSocketEndpoint::create_for_test(ioc_, config_, std::move(checkpoint));

    ASSERT_FALSE(endpoint.has_value());
    EXPECT_EQ(endpoint.error().operation, "inspect bound socket");
    struct stat preserved { };
    ASSERT_EQ(::lstat(config_.path.c_str(), &preserved), 0) << ::strerror(errno);
    EXPECT_TRUE(S_ISREG(preserved.st_mode));
    EXPECT_EQ(preserved.st_ino, replacement_inode);
}

TEST_F(UnixSocketEndpointTest, ReportsEntryDisappearedBeforeBoundEntryInspection)
{
    auto checkpoint = [&](SocketEndpointStage stage) -> std::expected<void, SocketEndpointError> {
        if (stage == SocketEndpointStage::BOUND_UNINSPECTED && ::unlink(config_.path.c_str()) != 0) {
            return std::unexpected(SocketEndpointError { "test setup", config_.path, "failed to remove bound socket" });
        }
        return { };
    };

    const auto endpoint = UnixSocketEndpoint::create_for_test(ioc_, config_, std::move(checkpoint));

    ASSERT_FALSE(endpoint.has_value());
    EXPECT_EQ(endpoint.error().operation, "inspect bound socket");
    EXPECT_EQ(endpoint.error().detail, "bound socket entry disappeared before identity was recorded");
    EXPECT_FALSE(std::filesystem::exists(config_.path));
}

TEST_F(UnixSocketEndpointTest, CleansUpAfterFailureAtEveryPostBindStage)
{
    constexpr std::array STAGES { SocketEndpointStage::BOUND, SocketEndpointStage::OWNED, SocketEndpointStage::MODE_SET,
        SocketEndpointStage::VERIFIED, SocketEndpointStage::LISTENING };

    for (const auto failed_stage : STAGES) {
        auto checkpoint = [failed_stage](SocketEndpointStage stage) -> std::expected<void, SocketEndpointError> {
            if (stage == failed_stage) {
                return std::unexpected(SocketEndpointError { "test checkpoint", "api.sock", "injected failure" });
            }
            return { };
        };

        const auto endpoint = UnixSocketEndpoint::create_for_test(ioc_, config_, std::move(checkpoint));

        ASSERT_FALSE(endpoint.has_value());
        EXPECT_EQ(endpoint.error().operation, "test checkpoint");
        EXPECT_FALSE(std::filesystem::exists(config_.path));
    }
}
}
