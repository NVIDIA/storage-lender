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

#include "server.hpp"
#include "accept_error.hpp"
#include "detail/posix_raii.hpp"
#include "lender_error.hpp"
#include "logging.hpp"
#include "pci_address.hpp"
#include "protocol.hpp"

#include "client.pb.h"
#include "storage_lender/wire/framing.hpp"
#include "wire.pb.h"

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>

#include <algorithm>
#include <chrono>
#include <ranges>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <variant>

namespace asio = boost::asio;

using nvidia::storage_lender::v1::CloseDeviceRequest;
using nvidia::storage_lender::v1::CreateCompletionQueueRequest;
using nvidia::storage_lender::v1::CreateCompletionQueueResponse;
using nvidia::storage_lender::v1::CreateSubmissionQueueRequest;
using nvidia::storage_lender::v1::CreateSubmissionQueueResponse;
using nvidia::storage_lender::v1::DeleteCompletionQueueRequest;
using nvidia::storage_lender::v1::DeleteSubmissionQueueRequest;
using nvidia::storage_lender::v1::DeviceInfoRequest;
using nvidia::storage_lender::v1::DeviceInfoResponse;
using nvidia::storage_lender::v1::MapBufferRequest;
using nvidia::storage_lender::v1::MapBufferResponse;
using nvidia::storage_lender::v1::MethodId;
using nvidia::storage_lender::v1::OpenDeviceRequest;
using nvidia::storage_lender::v1::OpenDeviceResponse;
using nvidia::storage_lender::v1::TransferFdResponse;
using nvidia::storage_lender::v1::UnmapBufferRequest;
using nvidia::storage_lender::wire::v1::Request;
using nvidia::storage_lender::wire::v1::Response;

namespace {
using storage_lender::server_detail::UniqueFd;

constexpr uint64_t CQE_SIZE = 16;
constexpr uint64_t SQE_SIZE = 64;
constexpr std::chrono::milliseconds ACCEPT_BACKOFF { 50 };

Response make_error(StatusCode code, const std::string& msg)
{
    Response resp;
    resp.set_status_code(code);
    resp.set_error_message(msg);
    return resp;
}

Response make_ok()
{
    Response resp;
    resp.set_status_code(StatusCode::OK);
    return resp;
}

Response make_ok(const google::protobuf::MessageLite& payload_msg)
{
    Response resp;
    resp.set_status_code(StatusCode::OK);
    resp.set_payload(payload_msg.SerializeAsString());
    return resp;
}

std::expected<void, LenderError> validate_queue_buffer(
    const BufferManager& buf_manager, uint64_t iova, uint32_t queue_size, uint64_t entry_size)
{
    auto size = buf_manager.buf_size(iova);
    if (!size) {
        return std::unexpected(size.error());
    }
    if (queue_size < 2 || queue_size > (*size / entry_size)) {
        return std::unexpected(LenderError::INVALID_ARGUMENT);
    }
    return { };
}

std::expected<PeerCredentials, LenderError> read_peer_credentials(int socket_fd)
{
    struct ucred credentials { };
    socklen_t credentials_size = sizeof(credentials);
    if (::getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_size) != 0
        || credentials_size != sizeof(credentials)) {
        return std::unexpected(LenderError::INTERNAL);
    }
    return PeerCredentials {
        .pid = credentials.pid,
        .uid = credentials.uid,
        .gid = credentials.gid,
    };
}

std::string_view quota_resource_name(QuotaResource resource)
{
    switch (resource) {
    case QuotaResource::SESSIONS:
        return "sessions";
    case QuotaResource::TRANSFERRED_FDS:
        return "transferred_fds";
    case QuotaResource::MAPPED_BUFFERS:
        return "mapped_buffers";
    case QuotaResource::MAX_BUFFER_BYTES:
        return "max_buffer_bytes";
    case QuotaResource::MAPPED_BYTES:
        return "mapped_bytes";
    case QuotaResource::DEVICE_HANDLES:
        return "device_handles";
    case QuotaResource::COMPLETION_QUEUES:
        return "completion_queues";
    case QuotaResource::SUBMISSION_QUEUES:
        return "submission_queues";
    }
    return "unknown";
}

std::string_view quota_scope_name(QuotaScope scope) { return scope == QuotaScope::PRINCIPAL ? "principal" : "global"; }

std::string format_current(const std::optional<uint64_t>& current)
{
    return current ? std::to_string(*current) : "n/a";
}

std::string format_limit(const QuotaLimit& limit) { return limit ? std::to_string(*limit) : "unlimited"; }

std::string changed_socket_fields(const SocketConfig& current, const SocketConfig& candidate)
{
    std::string changed;
    auto append_changed = [&changed](std::string_view field) {
        if (!changed.empty()) {
            changed += ", ";
        }
        changed += field;
    };
    if (candidate.path != current.path) {
        append_changed("path");
    }
    if (candidate.owner != current.owner) {
        append_changed("owner");
    }
    if (candidate.group != current.group) {
        append_changed("group");
    }
    if (candidate.mode != current.mode) {
        append_changed("mode");
    }
    return changed;
}

} // anonymous namespace

StorageLenderServer::ClientSession::ClientSession(
    Socket socket, NvmeBackend& backend, PeerCredentials peer, PrincipalId principal_id, QuotaLease session_lease)
    : quota { std::move(session_lease) }
    , sock { std::move(socket) }
    , credentials { peer }
    , principal { std::move(principal_id) }
    , buf_manager { backend }
    , queue_manager { backend }
{
}

StorageLenderServer::ClientSession::~ClientSession()
{
    std::ranges::for_each(devices, [](DeviceHandle& device) { device.quota.mark_orphaned(); });
}

void StorageLenderServer::accept_loop(Yield yield)
{
    boost::system::error_code ec;
    for (;;) {
        Socket sock(api_endpoint_.acceptor().get_executor());
        api_endpoint_.acceptor().async_accept(sock, yield[ec]);
        if (ec) {
            switch (classify_accept_error(ec)) {
            case AcceptErrorAction::Stop:
                return; // acceptor cancelled or closed at shutdown (operation_aborted or bad_descriptor); no log.
            case AcceptErrorAction::Backoff: {
                log_error("accept_loop: {} (backing off {}ms)", ec.message(), ACCEPT_BACKOFF.count());
                asio::steady_timer timer(api_endpoint_.acceptor().get_executor());
                timer.expires_after(ACCEPT_BACKOFF);
                boost::system::error_code timer_ec;
                timer.async_wait(yield[timer_ec]);
                if (timer_ec == asio::error::operation_aborted) {
                    return; // shutdown during backoff
                }
                continue;
            }
            case AcceptErrorAction::Continue:
                log_error("accept_loop: {}", ec.message());
                continue;
            }
        }

        auto peer = credential_reader_(sock.native_handle());
        if (!peer) {
            log_error("accept_loop: failed to read peer credentials");
            sock.close(ec);
            continue;
        }

        auto principal = principal_resolver_.resolve(peer->uid, peer->gid);
        auto quota = quota_manager_.acquire(principal, QuotaAmounts { .sessions = 1 });
        if (!quota) {
            if (const auto* denial = std::get_if<QuotaExceeded>(&quota.error())) {
                log_quota_denial(*peer, principal, "session admission", *denial);
            } else {
                log_quota_accounting_overflow(
                    *peer, "session admission", std::get<QuotaAccountingOverflow>(quota.error()));
            }
            sock.close(ec);
            continue;
        }

        asio::spawn(api_endpoint_.acceptor().get_executor(),
            [this, sock = std::move(sock), peer = *peer, principal = std::move(principal), quota = std::move(*quota)](
                Yield yield) mutable {
                client_loop(std::move(sock), peer, std::move(principal), std::move(quota), yield);
            });
    }
}

void StorageLenderServer::shutdown()
{
    log_info("shutdown: stopping accept loop, cancelling {} active session(s)", active_sockets_.size());
    boost::system::error_code ec;
    api_endpoint_.acceptor().close(ec);
    management_server_.shutdown();
    std::ranges::for_each(active_sockets_, [&ec](Socket* sock) { sock->cancel(ec); });
}

void StorageLenderServer::request_reload()
{
    if (reload_pending_) {
        return;
    }

    reload_pending_ = true;
    boost::asio::post(ioc_, [this] {
        reload_pending_ = false;
        reload_config();
    });
}

void StorageLenderServer::reload_config()
{
    if (config_path_.empty()) {
        log_error("Configuration reload rejected: no configuration path; previous policy remains active");
        return;
    }

    auto candidate = ConfigLoader::load(config_path_);
    if (!candidate) {
        log_error("Configuration reload rejected for {} at {}: {}; previous policy remains active",
            candidate.error().source, candidate.error().path, candidate.error().message);
        return;
    }

    if (candidate->device_policy != device_policy_config_) {
        log_error("Configuration reload rejected: [devices] is restart-only; previous policy remains active");
        return;
    }

    if (candidate->api_socket != api_socket_config_) {
        log_error(
            "Configuration reload rejected: [sockets.api] fields {} are restart-only; previous policy remains active",
            changed_socket_fields(api_socket_config_, candidate->api_socket));
        return;
    }

    if (candidate->ctl_socket != ctl_socket_config_) {
        log_error(
            "Configuration reload rejected: [sockets.ctl] fields {} are restart-only; previous policy remains active",
            changed_socket_fields(ctl_socket_config_, candidate->ctl_socket));
        return;
    }

    const auto old_mode = quota_manager_.snapshot().mode;
    const auto old_log_level = logging_config_.level;
    const auto new_log_level = candidate->logging.level;
    auto replaced = quota_manager_.replace_policy(std::move(candidate->quota_policy));
    if (!replaced) {
        log_error("Configuration reload rejected for principal {}: {}; previous policy remains active",
            replaced.error().principal, replaced.error().message);
        return;
    }

    principal_resolver_ = std::move(candidate->principal_resolver);
    const auto updated = quota_manager_.snapshot();
    auto report_reload = [&] {
        log_info("Configuration reloaded: quota mode {} -> {}, logging {} -> {}, generation {}",
            std::to_underlying(old_mode), std::to_underlying(updated.mode), log_level_name(old_log_level),
            log_level_name(new_log_level), updated.generation);
    };
    const auto report_before_change
        = log_level_enabled(old_log_level, LogLevel::INFO) && !log_level_enabled(new_log_level, LogLevel::INFO);
    if (report_before_change) {
        report_reload();
    }

    logging_config_ = candidate->logging;
    logging::set_level(logging_config_.level);

    if (!report_before_change) {
        report_reload();
    }
}

UsageSnapshot StorageLenderServer::usage_snapshot() const { return quota_manager_.snapshot(); }

void StorageLenderServer::log_quota_denial(
    const PeerCredentials& peer, const PrincipalId& principal, std::string_view operation, const QuotaExceeded& denial)
{
    log_warn("quota denied: pid {} uid {} gid {} principal {} operation {} resource {} requested {} scope {}; "
             "principal current {} limit {}; global current {} limit {}",
        peer.pid, peer.uid, peer.gid, principal, operation, quota_resource_name(denial.resource), denial.requested,
        quota_scope_name(denial.scope), format_current(denial.principal_current), format_limit(denial.principal_limit),
        format_current(denial.global_current), format_limit(denial.global_limit));
}

void StorageLenderServer::log_quota_accounting_overflow(
    const PeerCredentials& peer, std::string_view operation, const QuotaAccountingOverflow& overflow)
{
    log_error("quota accounting capacity exceeded: pid {} uid {} gid {} principal {} operation {} resource {} "
              "requested {} current {} scope {}",
        peer.pid, peer.uid, peer.gid, overflow.principal, operation, quota_resource_name(overflow.resource),
        overflow.requested, overflow.current, quota_scope_name(overflow.scope));
}

StorageLenderServer::Response StorageLenderServer::quota_error_response(const ClientSession& session,
    std::string_view operation, const QuotaAcquireError& error, std::string_view denial_message)
{
    if (const auto* denial = std::get_if<QuotaExceeded>(&error)) {
        log_quota_denial(session.credentials, session.principal, operation, *denial);
        return make_error(StatusCode::RESOURCE_EXHAUSTED, std::string { denial_message });
    }
    log_quota_accounting_overflow(session.credentials, operation, std::get<QuotaAccountingOverflow>(error));
    return make_error(StatusCode::INTERNAL, "quota accounting capacity exceeded");
}

void StorageLenderServer::client_loop(
    Socket sock, PeerCredentials peer, PrincipalId principal, QuotaLease session_quota, Yield yield)
{
    ClientSession session { std::move(sock), backend_, peer, std::move(principal), std::move(session_quota) };
    active_sockets_.push_back(&session.sock);

    log_info("{} client connected", session);

    session.sleep_fn = [&session, yield](std::chrono::milliseconds ms) mutable {
        asio::steady_timer timer(session.sock.get_executor());
        timer.expires_after(ms);
        boost::system::error_code ec;
        timer.async_wait(yield[ec]);
    };

    while (true) {
        std::string frame_payload;
        if (!read_frame(session, frame_payload, yield)) {
            break;
        }

        Request req;
        if (!req.ParseFromString(frame_payload)) {
            log_warn("{} client_loop: failed to parse request", session);
            break;
        }

        const auto measurement = command_metrics_.begin(req.method());
        Response resp = dispatch(req, session, session.sleep_fn, yield);
        command_metrics_.complete(measurement, resp.status_code());
        if (!write_frame(session, resp, yield)) {
            break;
        }
    }

    std::erase(active_sockets_, &session.sock);
    cleanup_session(session);
}

bool StorageLenderServer::read_frame(ClientSession& session, std::string& payload, Yield yield)
{
    boost::system::error_code ec;
    storage_lender::wire::FrameHeader header { };
    asio::async_read(session.sock, asio::buffer(header.data(), header.size()), yield[ec]);
    if (ec) {
        log_warn("{} read_frame: {}", session, ec.message());
        return false;
    }
    uint32_t len = 0;
    if (!storage_lender::wire::decode_frame_header(header, len)) {
        log_warn("{} read_frame: oversized frame ({} bytes)", session, len);
        return false;
    }
    payload.resize(len);
    if (len > 0) {
        asio::async_read(session.sock, asio::buffer(payload.data(), len), yield[ec]);
        if (ec) {
            log_warn("{} read_frame: {}", session, ec.message());
            return false;
        }
    }
    return true;
}

bool StorageLenderServer::write_frame(ClientSession& session, const Response& resp, Yield yield)
{
    std::string payload_bytes = resp.SerializeAsString();
    storage_lender::wire::FrameHeader header { };
    if (!storage_lender::wire::encode_frame_header(payload_bytes.size(), header)) {
        log_warn("{} write_frame: oversized frame ({} bytes)", session, payload_bytes.size());
        return false;
    }
    boost::system::error_code ec;
    std::array<asio::const_buffer, 2> bufs {
        asio::buffer(header.data(), header.size()),
        asio::buffer(payload_bytes),
    };
    asio::async_write(session.sock, bufs, yield[ec]);
    if (ec) {
        log_warn("{} write_frame: {}", session, ec.message());
    }
    return !ec;
}

Response StorageLenderServer::dispatch(const Request& req, ClientSession& session, const SleepFn& sleep_fn, Yield yield)
{
    switch (static_cast<MethodId>(req.method())) {
    case MethodId::TRANSFER_FD:
        return handle_transfer_fd(session, yield);
    case MethodId::MAP_BUFFER:
        return handle_map_buffer(req.payload(), session);
    case MethodId::UNMAP_BUFFER:
        return handle_unmap_buffer(req.payload(), session);
    case MethodId::OPEN_DEVICE:
        return handle_open_device(req.payload(), session);
    case MethodId::CLOSE_DEVICE:
        return handle_close_device(req.payload(), session);
    case MethodId::GET_DEVICE_INFO:
        return handle_get_device_info(req.payload(), session);
    case MethodId::CREATE_COMPLETION_QUEUE:
        return handle_create_cq(req.payload(), session, sleep_fn);
    case MethodId::DELETE_COMPLETION_QUEUE:
        return handle_delete_cq(req.payload(), session, sleep_fn);
    case MethodId::CREATE_SUBMISSION_QUEUE:
        return handle_create_sq(req.payload(), session, sleep_fn);
    case MethodId::DELETE_SUBMISSION_QUEUE:
        return handle_delete_sq(req.payload(), session, sleep_fn);
    default:
        log_warn("{} dispatch: unknown method {}", session, req.method());
        return make_error(StatusCode::INVALID_ARGUMENT, "unknown method");
    }
}

void StorageLenderServer::cleanup_session(ClientSession& session)
{
    std::ranges::for_each(session.queue_manager.all_sq_ids(), [this, &session](uint32_t sq_id) {
        uint32_t dev_id = session.queue_manager.device_of_sq(sq_id);
        auto ctrlr_r = dev_manager_.get_ctrlr(dev_id);
        if (!ctrlr_r) {
            log_error("{} cleanup: get_ctrlr for sq {}: {}", session, sq_id, lender_error_message(ctrlr_r.error()));
            session.queue_manager.orphan_sq(sq_id);
            return;
        }
        auto r = session.queue_manager.delete_sq(*ctrlr_r, sq_id, session.sleep_fn);
        if (!r) {
            log_error("{} cleanup: delete_sq {}: {}", session, sq_id, lender_error_message(r.error()));
            session.queue_manager.orphan_sq(sq_id);
            return;
        }
        log_info("{} device id {} submission queue id {} deleted", session, dev_id, sq_id);
    });
    std::ranges::for_each(session.queue_manager.all_cq_ids(), [this, &session](uint32_t cq_id) {
        uint32_t dev_id = session.queue_manager.device_of_cq(cq_id);
        auto ctrlr_r = dev_manager_.get_ctrlr(dev_id);
        if (!ctrlr_r) {
            log_error("{} cleanup: get_ctrlr for cq {}: {}", session, cq_id, lender_error_message(ctrlr_r.error()));
            session.queue_manager.orphan_cq(cq_id);
            return;
        }
        auto r = session.queue_manager.delete_cq(*ctrlr_r, cq_id, session.sleep_fn);
        if (!r) {
            log_error("{} cleanup: delete_cq {}: {}", session, cq_id, lender_error_message(r.error()));
            session.queue_manager.orphan_cq(cq_id);
            return;
        }
        log_info("{} device id {} completion queue id {} deleted", session, dev_id, cq_id);
    });

    std::ranges::for_each(session.devices, [this, &session](DeviceHandle& device) {
        auto r = dev_manager_.close_device(device.id);
        if (!r) {
            log_error("{} cleanup: close_device {}: {}", session, device.id, lender_error_message(r.error()));
            device.quota.mark_orphaned();
            return;
        }
        log_info("{} device id {} closed", session, device.id);
    });
    session.devices.clear();

    session.buf_manager.cleanup_all();

    boost::system::error_code ec;
    session.sock.close(ec);
    if (ec) {
        log_error("{} cleanup: close client socket: {}", session, ec.message());
    }
    session.quota.reset();
    log_info("{} client disconnected, resources cleaned up", session);
}

Response StorageLenderServer::handle_transfer_fd(ClientSession& session, Yield yield)
{
    boost::system::error_code ec;
    session.sock.async_wait(Socket::wait_read, yield[ec]);
    if (ec) {
        log_warn("{} async_wait failed: {}", session, ec.message());
        return make_error(StatusCode::INTERNAL, "wait failed");
    }

    char dummy[1];
    struct iovec iov { .iov_base = dummy, .iov_len = sizeof(dummy) };
    alignas(struct cmsghdr) char cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg { };
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    ssize_t n = ::recvmsg(session.sock.native_handle(), &msg, 0);
    if (n < 0) {
        log_warn("{} recvmsg failed: {}", session, strerror(errno));
        return make_error(StatusCode::INTERNAL, "recvmsg failed");
    }

    auto close_received_fds = [&msg]() {
        for (auto* header = CMSG_FIRSTHDR(&msg); header != nullptr; header = CMSG_NXTHDR(&msg, header)) {
            if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS || header->cmsg_len < CMSG_LEN(0)) {
                continue;
            }
            const auto n_fds = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            std::ranges::for_each(std::views::iota(size_t { 0 }, n_fds), [header](size_t i) {
                int received_fd = -1;
                ::memcpy(&received_fd, CMSG_DATA(header) + (i * sizeof(int)), sizeof(int));
                if (received_fd >= 0) {
                    ::close(received_fd);
                }
            });
        }
    };

    if ((msg.msg_flags & MSG_CTRUNC) != 0) {
        close_received_fds();
        log_warn("{} transfer_fd: ancillary data was truncated", session);
        return make_error(StatusCode::INVALID_ARGUMENT, "truncated ancillary data");
    }

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
        log_warn("{} transfer_fd: no file descriptor in ancillary data", session);
        return make_error(StatusCode::INVALID_ARGUMENT, "no file descriptor received");
    }
    if (cmsg->cmsg_len != CMSG_LEN(sizeof(int)) || CMSG_NXTHDR(&msg, cmsg) != nullptr) {
        log_warn("{} transfer_fd: expected exactly one file descriptor", session);
        close_received_fds();
        return make_error(StatusCode::INVALID_ARGUMENT, "expected exactly one file descriptor");
    }

    int raw_fd = -1;
    ::memcpy(&raw_fd, CMSG_DATA(cmsg), sizeof(int));
    UniqueFd received_fd { raw_fd };

    auto validated_size = BufferManager::validate_fd(received_fd.get());
    if (!validated_size) {
        log_warn("{} transfer_fd: validate fd {} failed: {}", session, received_fd.get(),
            lender_error_message(validated_size.error()));
        return make_error(to_status_code(validated_size.error()), lender_error_message(validated_size.error()));
    }

    auto quota = quota_manager_.acquire(session.principal, QuotaAmounts { .transferred_fds = 1 });
    if (!quota) {
        return quota_error_response(session, "TransferFd", quota.error(), "transferred file descriptor quota exceeded");
    }

    auto registered = session.buf_manager.register_fd(received_fd.get(), *validated_size, std::move(*quota));
    if (!registered) {
        log_warn("{} transfer_fd: register fd {} failed: {}", session, received_fd.get(),
            lender_error_message(registered.error()));
        return make_error(to_status_code(registered.error()), lender_error_message(registered.error()));
    }
    const auto registered_fd = received_fd.release();
    log_info("{} fd {} registered id {}", session, registered_fd, *registered);

    TransferFdResponse resp_msg;
    resp_msg.set_fd_id(*registered);
    return make_ok(resp_msg);
}

Response StorageLenderServer::handle_map_buffer(const std::string& payload, ClientSession& session)
{
    MapBufferRequest req;
    if (!req.ParseFromString(payload)) {
        log_warn("{} map_buffer: parse error", session);
        return make_error(StatusCode::INVALID_ARGUMENT, "parse error");
    }

    auto valid = session.buf_manager.validate_mapping(req.fd_id(), req.size());
    if (!valid) {
        log_warn("{} map_buffer: {}", session, lender_error_message(valid.error()));
        return make_error(to_status_code(valid.error()), lender_error_message(valid.error()));
    }

    auto quota = quota_manager_.acquire_buffer(session.principal, req.size());
    if (!quota) {
        return quota_error_response(session, "MapBuffer", quota.error(), "mapped buffer quota exceeded");
    }

    auto r = session.buf_manager.map_buffer(req.fd_id(), req.size(), req.alignment(), std::move(*quota));
    if (!r) {
        log_warn("{} map_buffer: {}", session, lender_error_message(r.error()));
        return make_error(to_status_code(r.error()), lender_error_message(r.error()));
    }
    log_info("{} buffer fd id {} size {} alignment {} mapped iova {:x}", session, req.fd_id(), req.size(),
        req.alignment(), *r);

    MapBufferResponse resp_msg;
    resp_msg.set_iova(*r);
    return make_ok(resp_msg);
}

Response StorageLenderServer::handle_unmap_buffer(const std::string& payload, ClientSession& session)
{
    UnmapBufferRequest req;
    if (!req.ParseFromString(payload)) {
        log_warn("{} unmap_buffer: parse error", session);
        return make_error(StatusCode::INVALID_ARGUMENT, "parse error");
    }

    auto r = session.buf_manager.unmap_buffer(req.iova());
    if (!r) {
        log_warn("{} unmap_buffer: {}", session, lender_error_message(r.error()));
        return make_error(to_status_code(r.error()), lender_error_message(r.error()));
    }
    log_info("{} buffer iova {} unmapped", session, req.iova());

    return make_ok();
}

Response StorageLenderServer::handle_open_device(const std::string& payload, ClientSession& session)
{
    OpenDeviceRequest req;
    if (!req.ParseFromString(payload)) {
        log_warn("{} open_device: parse error", session);
        return make_error(StatusCode::INVALID_ARGUMENT, "parse error");
    }

    OpenDeviceMode mode = (req.open_mode() == nvidia::storage_lender::v1::EXCLUSIVE) ? OpenDeviceMode::EXCLUSIVE
                                                                                     : OpenDeviceMode::SHARED;

    if (!is_valid_pci_bdf(req.pci_address())) {
        log_warn("{} open_device: invalid PCI address {}", session, req.pci_address());
        return make_error(StatusCode::INVALID_ARGUMENT, "invalid PCI address");
    }

    auto quota = quota_manager_.acquire(session.principal, QuotaAmounts { .device_handles = 1 });
    if (!quota) {
        return quota_error_response(session, "OpenDevice", quota.error(), "device handle quota exceeded");
    }

    auto r = dev_manager_.open_device(req.pci_address(), mode);
    if (!r) {
        log_warn("{} open_device {} mode {}: {}", session, req.pci_address(), mode, lender_error_message(r.error()));
        return make_error(to_status_code(r.error()), lender_error_message(r.error()));
    }
    log_info("{} device {} mode {} opened id {}", session, req.pci_address(), mode, *r);

    session.devices.push_back(DeviceHandle { .id = *r, .quota = std::move(*quota) });

    OpenDeviceResponse resp_msg;
    resp_msg.set_device_id(*r);
    return make_ok(resp_msg);
}

Response StorageLenderServer::handle_close_device(const std::string& payload, ClientSession& session)
{
    CloseDeviceRequest req;
    if (!req.ParseFromString(payload)) {
        log_warn("{} close_device: parse error", session);
        return make_error(StatusCode::INVALID_ARGUMENT, "parse error");
    }

    auto device = std::ranges::find(session.devices, req.device_id(), &DeviceHandle::id);
    if (device == session.devices.end()) {
        log_warn("{} close_device {}: not owned by this session", session, req.device_id());
        return make_error(StatusCode::NOT_FOUND, "device not found");
    }

    if (session.queue_manager.has_queues_on_device(req.device_id())) {
        log_warn("{} close_device {}: queues still exist on device", session, req.device_id());
        return make_error(StatusCode::FAILED_PRECONDITION, "delete all queues before closing the device");
    }

    auto r = dev_manager_.close_device(req.device_id());
    if (!r) {
        log_warn("{} close_device {}: {}", session, req.device_id(), lender_error_message(r.error()));
        return make_error(to_status_code(r.error()), lender_error_message(r.error()));
    }
    log_info("{} device id {} closed", session, req.device_id());

    session.devices.erase(device);
    return make_ok();
}

Response StorageLenderServer::handle_get_device_info(const std::string& payload, const ClientSession& session)
{
    DeviceInfoRequest req;
    if (!req.ParseFromString(payload)) {
        log_warn("{} get_device_info: parse error", session);
        return make_error(StatusCode::INVALID_ARGUMENT, "parse error");
    }

    if (std::ranges::find(session.devices, req.device_id(), &DeviceHandle::id) == session.devices.end()) {
        log_warn("{} get_device_info {}: not owned by this session", session, req.device_id());
        return make_error(StatusCode::NOT_FOUND, "device not found");
    }

    auto r = dev_manager_.get_controller_info(req.device_id());
    if (!r) {
        log_warn("{} get_device_info {}: {}", session, req.device_id(), lender_error_message(r.error()));
        return make_error(to_status_code(r.error()), lender_error_message(r.error()));
    }

    DeviceInfoResponse resp_msg;
    resp_msg.set_model(r->model);
    resp_msg.set_pci_resource_path(r->pci_resource_path);
    resp_msg.set_max_queue_entries(r->max_queue_entries);
    resp_msg.set_page_size(r->page_size);
    resp_msg.set_num_io_queues(r->num_io_queues);
    std::ranges::for_each(r->namespaces, [&resp_msg](const auto& ns) {
        auto* ns_proto = resp_msg.add_namespaces();
        ns_proto->set_ns_id(ns.ns_id);
        ns_proto->set_block_size(ns.block_size);
        ns_proto->set_block_count(ns.block_count);
    });
    return make_ok(resp_msg);
}

Response StorageLenderServer::handle_create_cq(
    const std::string& payload, ClientSession& session, const SleepFn& sleep_fn)
{
    CreateCompletionQueueRequest req;
    if (!req.ParseFromString(payload)) {
        log_warn("{} create_cq: parse error", session);
        return make_error(StatusCode::INVALID_ARGUMENT, "parse error");
    }

    if (!session.buf_manager.has_iova(req.iova())) {
        log_warn("{} create_cq: unknown iova {:#x}", session, req.iova());
        return make_error(StatusCode::INVALID_ARGUMENT, "unknown iova");
    }
    if (auto r = validate_queue_buffer(session.buf_manager, req.iova(), req.queue_size(), CQE_SIZE); !r) {
        log_warn("{} create_cq: queue size {} does not fit in iova {:#x}", session, req.queue_size(), req.iova());
        return make_error(to_status_code(r.error()), lender_error_message(r.error()));
    }

    if (std::ranges::find(session.devices, req.device_id(), &DeviceHandle::id) == session.devices.end()) {
        log_warn("{} create_cq: device {} not owned by this session", session, req.device_id());
        return make_error(StatusCode::NOT_FOUND, "device not found");
    }

    auto ctrlr_r = dev_manager_.get_ctrlr(req.device_id());
    if (!ctrlr_r) {
        log_warn("{} create_cq: get_ctrlr {}: {}", session, req.device_id(), lender_error_message(ctrlr_r.error()));
        return make_error(to_status_code(ctrlr_r.error()), lender_error_message(ctrlr_r.error()));
    }

    auto quota = quota_manager_.acquire(session.principal, QuotaAmounts { .completion_queues = 1 });
    if (!quota) {
        return quota_error_response(session, "CreateCompletionQueue", quota.error(), "completion queue quota exceeded");
    }

    auto r = session.queue_manager.create_cq(
        *ctrlr_r, req.device_id(), req.iova(), req.queue_size(), sleep_fn, std::move(*quota));
    if (!r) {
        log_warn("{} create_cq device {}: {}", session, req.device_id(), lender_error_message(r.error()));
        return make_error(to_status_code(r.error()), lender_error_message(r.error()));
    }
    log_info("{} device id {} iova {:x} queue size {} completion queue created id {} db offset {}", session,
        req.device_id(), req.iova(), req.queue_size(), r->qid, r->db_offset);

    CreateCompletionQueueResponse resp_msg;
    resp_msg.set_cq_id(r->qid);
    resp_msg.set_cq_db_offset(r->db_offset);
    return make_ok(resp_msg);
}

Response StorageLenderServer::handle_delete_cq(
    const std::string& payload, ClientSession& session, const SleepFn& sleep_fn)
{
    DeleteCompletionQueueRequest req;
    if (!req.ParseFromString(payload)) {
        log_warn("{} delete_cq: parse error", session);
        return make_error(StatusCode::INVALID_ARGUMENT, "parse error");
    }

    if (!session.queue_manager.has_cq(req.cq_id())) {
        log_warn("{} delete_cq {}: not found", session, req.cq_id());
        return make_error(StatusCode::NOT_FOUND, "cq not found");
    }

    if (session.queue_manager.has_sqs_for_cq(req.cq_id())) {
        log_warn("{} delete_cq {}: submission queues still exist", session, req.cq_id());
        return make_error(StatusCode::FAILED_PRECONDITION, "submission queues still exist");
    }

    uint32_t dev_id = session.queue_manager.device_of_cq(req.cq_id());
    auto ctrlr_r = dev_manager_.get_ctrlr(dev_id);
    if (!ctrlr_r) {
        log_warn(
            "{} delete_cq {}: get_ctrlr {}: {}", session, req.cq_id(), dev_id, lender_error_message(ctrlr_r.error()));
        return make_error(to_status_code(ctrlr_r.error()), lender_error_message(ctrlr_r.error()));
    }

    auto r = session.queue_manager.delete_cq(*ctrlr_r, req.cq_id(), sleep_fn);
    if (!r) {
        log_warn("{} delete_cq {}: {}", session, req.cq_id(), lender_error_message(r.error()));
        return make_error(to_status_code(r.error()), lender_error_message(r.error()));
    }
    log_info("{} device id {} completion queue deleted", session, dev_id, req.cq_id());

    return make_ok();
}

Response StorageLenderServer::handle_create_sq(
    const std::string& payload, ClientSession& session, const SleepFn& sleep_fn)
{
    CreateSubmissionQueueRequest req;
    if (!req.ParseFromString(payload)) {
        log_warn("{} create_sq: parse error", session);
        return make_error(StatusCode::INVALID_ARGUMENT, "parse error");
    }

    if (!session.buf_manager.has_iova(req.iova())) {
        log_warn("{} create_sq: unknown iova {:#x}", session, req.iova());
        return make_error(StatusCode::INVALID_ARGUMENT, "unknown iova");
    }
    if (auto r = validate_queue_buffer(session.buf_manager, req.iova(), req.queue_size(), SQE_SIZE); !r) {
        log_warn("{} create_sq: queue size {} does not fit in iova {:#x}", session, req.queue_size(), req.iova());
        return make_error(to_status_code(r.error()), lender_error_message(r.error()));
    }

    if (!session.queue_manager.has_cq(req.cq_id())) {
        log_warn("{} create_sq: cq {} not found", session, req.cq_id());
        return make_error(StatusCode::NOT_FOUND, "cq not found");
    }

    if (std::ranges::find(session.devices, req.device_id(), &DeviceHandle::id) == session.devices.end()) {
        log_warn("{} create_sq: device {} not owned by this session", session, req.device_id());
        return make_error(StatusCode::NOT_FOUND, "device not found");
    }

    auto ctrlr_r = dev_manager_.get_ctrlr(req.device_id());
    if (!ctrlr_r) {
        log_warn("{} create_sq: get_ctrlr {}: {}", session, req.device_id(), lender_error_message(ctrlr_r.error()));
        return make_error(to_status_code(ctrlr_r.error()), lender_error_message(ctrlr_r.error()));
    }

    auto quota = quota_manager_.acquire(session.principal, QuotaAmounts { .submission_queues = 1 });
    if (!quota) {
        return quota_error_response(session, "CreateSubmissionQueue", quota.error(), "submission queue quota exceeded");
    }

    auto r = session.queue_manager.create_sq(
        *ctrlr_r, req.device_id(), req.cq_id(), req.iova(), req.queue_size(), sleep_fn, std::move(*quota));
    if (!r) {
        log_warn("{} create_sq device {}: {}", session, req.device_id(), lender_error_message(r.error()));
        return make_error(to_status_code(r.error()), lender_error_message(r.error()));
    }
    log_info("{} device id {} cq id {}, iova {:x} queue size {} submission queue created id {} "
             "db offset {}",
        session, req.device_id(), req.cq_id(), req.iova(), req.queue_size(), r->qid, r->db_offset);

    CreateSubmissionQueueResponse resp_msg;
    resp_msg.set_sq_id(r->qid);
    resp_msg.set_sq_db_offset(r->db_offset);
    return make_ok(resp_msg);
}

Response StorageLenderServer::handle_delete_sq(
    const std::string& payload, ClientSession& session, const SleepFn& sleep_fn)
{
    DeleteSubmissionQueueRequest req;
    if (!req.ParseFromString(payload)) {
        log_warn("{} delete_sq: parse error", session);
        return make_error(StatusCode::INVALID_ARGUMENT, "parse error");
    }

    uint32_t dev_id = session.queue_manager.device_of_sq(req.sq_id());
    if (dev_id == 0) {
        log_warn("{} delete_sq {}: not found", session, req.sq_id());
        return make_error(StatusCode::NOT_FOUND, "sq not found");
    }

    auto ctrlr_r = dev_manager_.get_ctrlr(dev_id);
    if (!ctrlr_r) {
        log_warn(
            "{} delete_sq {}: get_ctrlr {}: {}", session, req.sq_id(), dev_id, lender_error_message(ctrlr_r.error()));
        return make_error(to_status_code(ctrlr_r.error()), lender_error_message(ctrlr_r.error()));
    }

    auto r = session.queue_manager.delete_sq(*ctrlr_r, req.sq_id(), sleep_fn);
    if (!r) {
        log_warn("{} delete_sq {}: {}", session, req.sq_id(), lender_error_message(r.error()));
        return make_error(to_status_code(r.error()), lender_error_message(r.error()));
    }
    log_info("{} device id {} submission queue deleted", session, dev_id, req.sq_id());

    return make_ok();
}

StorageLenderServer::StorageLenderServer(asio::io_context& ioc, UnixSocketEndpoint api_endpoint,
    UnixSocketEndpoint ctl_endpoint, NvmeBackend& backend, DeviceManager& dm, ServerConfig config,
    std::string config_path, PeerCredentialReader credential_reader)
    : ioc_ { ioc }
    , config_path_ { std::move(config_path) }
    , api_socket_config_ { std::move(config.api_socket) }
    , ctl_socket_config_ { std::move(config.ctl_socket) }
    , device_policy_config_ { std::move(config.device_policy) }
    , logging_config_ { config.logging }
    , quota_manager_ { std::move(config.quota_policy) }
    , principal_resolver_ { std::move(config.principal_resolver) }
    , credential_reader_ { credential_reader ? std::move(credential_reader)
                                             : PeerCredentialReader { read_peer_credentials } }
    , api_endpoint_ { std::move(api_endpoint) }
    , command_metrics_ { ioc.get_executor() }
    , management_server_ { std::move(ctl_endpoint), quota_manager_, command_metrics_, credential_reader_ }
    , backend_ { backend }
    , dev_manager_ { dm }
{
    logging::set_level(logging_config_.level);
    asio::spawn(api_endpoint_.acceptor().get_executor(), [this](Yield yield) { accept_loop(yield); });

    log_info("Server listening on API {} and ctl {}", api_socket_config_.path, ctl_socket_config_.path);
}
