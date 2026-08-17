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
#include "logging.hpp"

#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
using storage_lender::server_detail::ScopedUmask;
using storage_lender::server_detail::UniqueFd;

SocketEndpointError errno_error(std::string operation, std::string target, int error)
{
    return SocketEndpointError { std::move(operation), std::move(target), ::strerror(error) };
}

SocketEndpointError boost_error(std::string operation, std::string target, const boost::system::error_code& error)
{
    return SocketEndpointError { std::move(operation), std::move(target), error.message() };
}

std::expected<void, SocketEndpointError> validate_config(const SocketConfig& config)
{
    if (const auto error = socket_path_validation_error(config.path); error.has_value()) {
        return std::unexpected(SocketEndpointError { "validate socket path", config.path, *error });
    }
    if (config.owner.empty()) {
        return std::unexpected(
            SocketEndpointError { "validate socket owner", "owner", "account name must not be empty" });
    }
    if (config.group.empty()) {
        return std::unexpected(
            SocketEndpointError { "validate socket group", "group", "account name must not be empty" });
    }
    if (!is_allowed_socket_mode(config.mode)) {
        return std::unexpected(SocketEndpointError { "validate socket mode", config.path, "mode is not allowed" });
    }
    return { };
}

std::size_t account_buffer_size(int name)
{
    constexpr std::size_t FALLBACK_SIZE = 1024;
    const auto configured_size = ::sysconf(name);
    return configured_size > 0 ? static_cast<std::size_t>(configured_size) : FALLBACK_SIZE;
}

std::expected<uid_t, SocketEndpointError> resolve_owner(const std::string& name)
{
    auto buffer = std::vector<char>(account_buffer_size(_SC_GETPW_R_SIZE_MAX));
    for (;;) {
        struct passwd account { };
        struct passwd* result = nullptr;
        const auto error = ::getpwnam_r(name.c_str(), &account, buffer.data(), buffer.size(), &result);
        if (error == ERANGE) {
            if (buffer.size() > std::numeric_limits<std::size_t>::max() / 2) {
                return std::unexpected(
                    SocketEndpointError { "resolve socket owner", name, "account record is too large" });
            }
            buffer.resize(buffer.size() * 2);
            continue;
        }
        if (error != 0) {
            return std::unexpected(errno_error("resolve socket owner", name, error));
        }
        if (result == nullptr) {
            return std::unexpected(SocketEndpointError { "resolve socket owner", name, "unknown account" });
        }
        return account.pw_uid;
    }
}

std::expected<gid_t, SocketEndpointError> resolve_group(const std::string& name)
{
    auto buffer = std::vector<char>(account_buffer_size(_SC_GETGR_R_SIZE_MAX));
    for (;;) {
        struct group account { };
        struct group* result = nullptr;
        const auto error = ::getgrnam_r(name.c_str(), &account, buffer.data(), buffer.size(), &result);
        if (error == ERANGE) {
            if (buffer.size() > std::numeric_limits<std::size_t>::max() / 2) {
                return std::unexpected(
                    SocketEndpointError { "resolve socket group", name, "account record is too large" });
            }
            buffer.resize(buffer.size() * 2);
            continue;
        }
        if (error != 0) {
            return std::unexpected(errno_error("resolve socket group", name, error));
        }
        if (result == nullptr) {
            return std::unexpected(SocketEndpointError { "resolve socket group", name, "unknown account" });
        }
        return account.gr_gid;
    }
}

std::expected<void, SocketEndpointError> validate_process_accounts(
    const SocketConfig& config, uid_t owner_uid, gid_t group_gid)
{
    const auto effective_uid = ::geteuid();
    if (effective_uid == 0) {
        return { };
    }
    if (owner_uid != effective_uid) {
        return std::unexpected(
            SocketEndpointError { "validate socket owner", config.owner, "owner must match the effective user" });
    }
    if (group_gid == ::getegid()) {
        return { };
    }

    const auto group_count = ::getgroups(0, nullptr);
    if (group_count < 0) {
        const auto error = errno;
        return std::unexpected(errno_error("read supplementary groups", config.group, error));
    }
    auto groups = std::vector<gid_t>(static_cast<std::size_t>(group_count));
    if (group_count > 0 && ::getgroups(group_count, groups.data()) < 0) {
        const auto error = errno;
        return std::unexpected(errno_error("read supplementary groups", config.group, error));
    }
    if (std::find(groups.begin(), groups.end(), group_gid) == groups.end()) {
        return std::unexpected(SocketEndpointError {
            "validate socket group", config.group, "group must belong to the effective process" });
    }
    return { };
}

struct ParentDirectory {
    UniqueFd fd;
    std::string filename;
};

std::expected<ParentDirectory, SocketEndpointError> open_parent_directory(
    const std::string& socket_path, uid_t effective_uid)
{
    const auto final_separator = socket_path.rfind('/');
    auto filename = socket_path.substr(final_separator + 1);
    const auto parent_path = final_separator == 0 ? std::string { } : socket_path.substr(1, final_separator - 1);

    std::vector<std::string> components;
    for (std::size_t begin = 0; begin < parent_path.size();) {
        const auto end = parent_path.find('/', begin);
        auto component = parent_path.substr(begin, end == std::string::npos ? parent_path.size() - begin : end - begin);
        if (!component.empty()) {
            components.push_back(std::move(component));
        }
        begin = end == std::string::npos ? parent_path.size() : end + 1;
    }

    UniqueFd current { ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
    if (current.get() < 0) {
        const auto error = errno;
        return std::unexpected(errno_error("open socket directory", "/", error));
    }

    struct stat metadata { };
    if (::fstat(current.get(), &metadata) != 0) {
        const auto error = errno;
        return std::unexpected(errno_error("inspect socket directory", "/", error));
    }
    const auto root_role = components.empty() ? socket_endpoint_detail::DirectoryRole::PARENT
                                              : socket_endpoint_detail::DirectoryRole::ANCESTOR;
    if (auto validated = socket_endpoint_detail::validate_directory_metadata(metadata, effective_uid, root_role, "/");
        !validated) {
        return std::unexpected(validated.error());
    }

    std::string current_path;
    for (std::size_t index = 0; index < components.size(); ++index) {
        current_path += "/" + components[index];
        UniqueFd next { ::openat(
            current.get(), components[index].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) };
        if (next.get() < 0) {
            const auto error = errno;
            return std::unexpected(errno_error("open socket directory", current_path, error));
        }
        if (::fstat(next.get(), &metadata) != 0) {
            const auto error = errno;
            return std::unexpected(errno_error("inspect socket directory", current_path, error));
        }
        const auto role = index + 1 == components.size() ? socket_endpoint_detail::DirectoryRole::PARENT
                                                         : socket_endpoint_detail::DirectoryRole::ANCESTOR;
        if (auto validated
            = socket_endpoint_detail::validate_directory_metadata(metadata, effective_uid, role, current_path);
            !validated) {
            return std::unexpected(validated.error());
        }
        current = std::move(next);
    }

    return ParentDirectory { std::move(current), std::move(filename) };
}
}

std::string SocketEndpointError::message() const { return operation + " " + target + ": " + detail; }

std::expected<void, SocketEndpointError> socket_endpoint_detail::validate_directory_metadata(
    const struct stat& metadata, uid_t effective_uid, DirectoryRole role, std::string_view path)
{
    const bool owner_allowed = metadata.st_uid == 0 || metadata.st_uid == effective_uid;
    const bool writable = (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0;
    const bool root_sticky = metadata.st_uid == 0 && (metadata.st_mode & S_ISVTX) != 0;

    if (!S_ISDIR(metadata.st_mode)) {
        return std::unexpected(
            SocketEndpointError { "validate directory type", std::string { path }, "component is not a directory" });
    }
    if (role == DirectoryRole::PARENT) {
        if (metadata.st_uid != effective_uid || writable) {
            return std::unexpected(SocketEndpointError { "validate socket parent", std::string { path },
                "parent must belong to the effective user and deny group/other writes" });
        }
    } else if (!owner_allowed || (writable && !root_sticky)) {
        return std::unexpected(SocketEndpointError {
            "validate socket ancestor", std::string { path }, "ancestor has unsafe ownership or write permissions" });
    }
    return { };
}

std::expected<UnixSocketEndpoint, SocketEndpointError> UnixSocketEndpoint::create(
    boost::asio::io_context& ioc, const SocketConfig& config)
{
    if (auto validated = validate_config(config); !validated) {
        return std::unexpected(validated.error());
    }
    return create_for_test(
        ioc, config, [](SocketEndpointStage) -> std::expected<void, SocketEndpointError> { return { }; });
}

std::expected<UnixSocketEndpoint, SocketEndpointError> UnixSocketEndpoint::create_for_test(
    boost::asio::io_context& ioc, const SocketConfig& config, SocketEndpointCheckpoint checkpoint)
{
    if (auto validated = validate_config(config); !validated) {
        return std::unexpected(validated.error());
    }

    const auto owner = resolve_owner(config.owner);
    if (!owner) {
        return std::unexpected(owner.error());
    }
    const auto group = resolve_group(config.group);
    if (!group) {
        return std::unexpected(group.error());
    }
    if (auto validated = validate_process_accounts(config, *owner, *group); !validated) {
        return std::unexpected(validated.error());
    }

    auto parent = open_parent_directory(config.path, ::geteuid());
    if (!parent) {
        return std::unexpected(parent.error());
    }

    UnixSocketEndpoint endpoint { ioc };
    endpoint.parent_fd_ = parent->fd.release();
    endpoint.filename_ = std::move(parent->filename);

    struct stat existing { };
    if (::fstatat(endpoint.parent_fd_, endpoint.filename_.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        return std::unexpected(SocketEndpointError { "check socket path", config.path, "entry already exists" });
    }
    if (const auto error = errno; error != ENOENT) {
        return std::unexpected(errno_error("check socket path", config.path, error));
    }

    boost::system::error_code ec;
    endpoint.acceptor_.open(boost::asio::local::stream_protocol(), ec);
    if (ec) {
        return std::unexpected(boost_error("open socket acceptor", config.path, ec));
    }
    {
        ScopedUmask restrictive_umask { 0077 };
        endpoint.acceptor_.bind(boost::asio::local::stream_protocol::endpoint { config.path }, ec);
    }
    if (ec) {
        return std::unexpected(boost_error("bind socket", config.path, ec));
    }

    // A post-bind failure cannot be returned until cleanup has a pathname identity to guard.
    std::optional<SocketEndpointError> pending_checkpoint_error;
    if (auto checked = checkpoint(SocketEndpointStage::BOUND_UNINSPECTED); !checked) {
        pending_checkpoint_error = std::move(checked.error());
    }

    struct stat bound { };
    int inspect_result;
    do {
        inspect_result = ::fstatat(endpoint.parent_fd_, endpoint.filename_.c_str(), &bound, AT_SYMLINK_NOFOLLOW);
    } while (inspect_result != 0 && errno == EINTR);
    if (inspect_result != 0) {
        const auto error = errno;
        if (error == ENOENT) {
            return std::unexpected(SocketEndpointError {
                "inspect bound socket", config.path, "bound socket entry disappeared before identity was recorded" });
        }
        return std::unexpected(SocketEndpointError { "inspect bound socket", config.path,
            std::string { ::strerror(error) } + "; bound socket identity is unavailable, pathname preserved" });
    }
    endpoint.socket_device_ = bound.st_dev;
    endpoint.socket_inode_ = bound.st_ino;
    endpoint.owns_socket_ = true;
    if (pending_checkpoint_error.has_value()) {
        return std::unexpected(std::move(*pending_checkpoint_error));
    }
    if (!S_ISSOCK(bound.st_mode)) {
        return std::unexpected(
            SocketEndpointError { "inspect bound socket", config.path, "bound entry is not a socket" });
    }
    if (auto checked = checkpoint(SocketEndpointStage::BOUND); !checked) {
        return std::unexpected(checked.error());
    }

    if (::fchownat(endpoint.parent_fd_, endpoint.filename_.c_str(), *owner, *group, AT_SYMLINK_NOFOLLOW) != 0) {
        const auto error = errno;
        return std::unexpected(errno_error("set socket ownership", config.path, error));
    }
    if (auto checked = checkpoint(SocketEndpointStage::OWNED); !checked) {
        return std::unexpected(checked.error());
    }

    if (::fchmodat(endpoint.parent_fd_, endpoint.filename_.c_str(), config.mode, 0) != 0) {
        const auto error = errno;
        return std::unexpected(errno_error("set socket mode", config.path, error));
    }
    if (auto checked = checkpoint(SocketEndpointStage::MODE_SET); !checked) {
        return std::unexpected(checked.error());
    }

    struct stat secured { };
    if (::fstatat(endpoint.parent_fd_, endpoint.filename_.c_str(), &secured, AT_SYMLINK_NOFOLLOW) != 0) {
        const auto error = errno;
        return std::unexpected(errno_error("verify socket metadata", config.path, error));
    }
    if (!S_ISSOCK(secured.st_mode) || secured.st_uid != *owner || secured.st_gid != *group
        || (secured.st_mode & ACCESSPERMS) != config.mode || secured.st_dev != endpoint.socket_device_
        || secured.st_ino != endpoint.socket_inode_) {
        return std::unexpected(SocketEndpointError {
            "verify socket metadata", config.path, "created socket does not match requested metadata" });
    }
    if (auto checked = checkpoint(SocketEndpointStage::VERIFIED); !checked) {
        return std::unexpected(checked.error());
    }

    endpoint.acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        return std::unexpected(boost_error("listen on socket", config.path, ec));
    }
    if (auto checked = checkpoint(SocketEndpointStage::LISTENING); !checked) {
        return std::unexpected(checked.error());
    }

    return std::expected<UnixSocketEndpoint, SocketEndpointError> { std::in_place, std::move(endpoint) };
}

UnixSocketEndpoint::~UnixSocketEndpoint() { cleanup(); }

UnixSocketEndpoint::UnixSocketEndpoint(UnixSocketEndpoint&& other) noexcept
    : acceptor_ { std::move(other.acceptor_) }
    , parent_fd_ { std::exchange(other.parent_fd_, -1) }
    , filename_ { std::exchange(other.filename_, { }) }
    , socket_device_ { std::exchange(other.socket_device_, { }) }
    , socket_inode_ { std::exchange(other.socket_inode_, { }) }
    , owns_socket_ { std::exchange(other.owns_socket_, false) }
{
}

UnixSocketEndpoint::Acceptor& UnixSocketEndpoint::acceptor() { return acceptor_; }

UnixSocketEndpoint::UnixSocketEndpoint(boost::asio::io_context& ioc)
    : acceptor_ { ioc }
{
}

void UnixSocketEndpoint::cleanup() noexcept
{
    boost::system::error_code ec;
    acceptor_.close(ec);

    if (owns_socket_) {
        struct stat current { };
        if (::fstatat(parent_fd_, filename_.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0) {
            if (S_ISSOCK(current.st_mode) && current.st_dev == socket_device_ && current.st_ino == socket_inode_) {
                if (::unlinkat(parent_fd_, filename_.c_str(), 0) != 0) {
                    const auto error = errno;
                    log_error("socket cleanup {}: {}", filename_, ::strerror(error));
                }
            } else {
                log_warn("socket cleanup preserved replacement {}", filename_);
            }
        } else {
            const auto error = errno;
            if (error != ENOENT) {
                log_error("socket cleanup stat {}: {}", filename_, ::strerror(error));
            }
        }
    }
    owns_socket_ = false;
    if (parent_fd_ >= 0) {
        ::close(parent_fd_);
        parent_fd_ = -1;
    }
}
