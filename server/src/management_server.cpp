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

#include "management_server.hpp"

#include "accept_error.hpp"
#include "ctl.pb.h"
#include "logging.hpp"
#include "storage_lender/wire/framing.hpp"
#include "wire.pb.h"

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <set>
#include <string_view>
#include <utility>

namespace asio = boost::asio;

namespace {

using CtlMethod = nvidia::storage_lender::ctl::v1::MethodId;
using CtlQuotaLimits = nvidia::storage_lender::ctl::v1::QuotaLimits;
using CtlQuotaUsage = nvidia::storage_lender::ctl::v1::QuotaUsage;
using CtlUsageTotals = nvidia::storage_lender::ctl::v1::UsageTotals;
using Request = nvidia::storage_lender::wire::v1::Request;
using Response = nvidia::storage_lender::wire::v1::Response;
using StatusCode = nvidia::storage_lender::wire::v1::StatusCode;

constexpr std::chrono::milliseconds ACCEPT_BACKOFF { 50 };

Response make_error(StatusCode status, std::string_view message)
{
    Response response;
    response.set_status_code(status);
    response.set_error_message(message.data(), message.size());
    return response;
}

Response make_ok(const google::protobuf::MessageLite& payload)
{
    Response response;
    response.set_status_code(StatusCode::OK);
    response.set_payload(payload.SerializeAsString());
    return response;
}

void copy_usage(CtlQuotaUsage& destination, const QuotaAmounts& source)
{
    destination.set_sessions(source.sessions);
    destination.set_transferred_fds(source.transferred_fds);
    destination.set_mapped_buffers(source.mapped_buffers);
    destination.set_mapped_bytes(source.mapped_bytes);
    destination.set_device_handles(source.device_handles);
    destination.set_completion_queues(source.completion_queues);
    destination.set_submission_queues(source.submission_queues);
}

void copy_usage(CtlUsageTotals& destination, const UsageTotals& source)
{
    copy_usage(*destination.mutable_active(), source.active);
    copy_usage(*destination.mutable_orphan(), source.orphan);
}

void copy_limits(CtlQuotaLimits& destination, const QuotaLimits& source)
{
    if (source.sessions) {
        destination.set_sessions(*source.sessions);
    }
    if (source.transferred_fds) {
        destination.set_transferred_fds(*source.transferred_fds);
    }
    if (source.mapped_buffers) {
        destination.set_mapped_buffers(*source.mapped_buffers);
    }
    if (source.max_buffer_bytes) {
        destination.set_max_buffer_bytes(*source.max_buffer_bytes);
    }
    if (source.mapped_bytes) {
        destination.set_mapped_bytes(*source.mapped_bytes);
    }
    if (source.device_handles) {
        destination.set_device_handles(*source.device_handles);
    }
    if (source.completion_queues) {
        destination.set_completion_queues(*source.completion_queues);
    }
    if (source.submission_queues) {
        destination.set_submission_queues(*source.submission_queues);
    }
}

nvidia::storage_lender::ctl::v1::QuotaMode ctl_quota_mode(QuotaMode mode)
{
    return mode == QuotaMode::ENFORCED ? nvidia::storage_lender::ctl::v1::QUOTA_MODE_ENFORCED
                                       : nvidia::storage_lender::ctl::v1::QUOTA_MODE_UNLIMITED;
}

nvidia::storage_lender::ctl::v1::GetQuotaStateResponse make_quota_state_response(const UsageSnapshot& snapshot)
{
    nvidia::storage_lender::ctl::v1::GetQuotaStateResponse response;
    response.set_generation(snapshot.generation);
    response.set_mode(ctl_quota_mode(snapshot.mode));
    copy_usage(*response.mutable_global_usage(), snapshot.global_usage);
    copy_limits(*response.mutable_global_limits(), snapshot.global_limits);
    copy_limits(*response.mutable_default_principal_limits(), snapshot.default_limits);

    std::set<PrincipalId> principals;
    for (const auto& [principal, usage] : snapshot.principal_usage) {
        static_cast<void>(usage);
        principals.insert(principal);
    }
    for (const auto& [principal, limits] : snapshot.named_limits) {
        static_cast<void>(limits);
        principals.insert(principal);
    }

    for (const auto& principal : principals) {
        auto* state = response.add_principals();
        state->set_principal(principal);

        const auto usage = snapshot.principal_usage.find(principal);
        if (usage != snapshot.principal_usage.end()) {
            copy_usage(*state->mutable_usage(), usage->second);
        }

        const auto limits = snapshot.named_limits.find(principal);
        const auto named_limit_override = limits != snapshot.named_limits.end();
        copy_limits(*state->mutable_limits(), named_limit_override ? limits->second : snapshot.default_limits);
        state->set_named_limit_override(named_limit_override);
    }

    return response;
}

nvidia::storage_lender::ctl::v1::GetCommandLatencyResponse make_command_latency_response(
    const CommandMetricsSnapshot& snapshot)
{
    nvidia::storage_lender::ctl::v1::GetCommandLatencyResponse response;
    response.set_capacity_per_command(snapshot.capacity_per_command);
    response.set_process_started_unix_ms(snapshot.process_started_unix_ms);
    for (const auto& source : snapshot.commands) {
        auto* destination = response.add_commands();
        destination->set_command(source.command.data(), source.command.size());
        destination->set_samples(source.samples);
        destination->set_errors(source.errors);
        destination->set_peak_in_flight(source.peak_in_flight);
        if (source.p50_us) {
            destination->set_p50_us(*source.p50_us);
        }
        if (source.p90_us) {
            destination->set_p90_us(*source.p90_us);
        }
        if (source.p99_us) {
            destination->set_p99_us(*source.p99_us);
        }
        if (source.max_us) {
            destination->set_max_us(*source.max_us);
        }
        if (source.event_loop_lag_p90_us) {
            destination->set_event_loop_lag_p90_us(*source.event_loop_lag_p90_us);
        }
        if (source.event_loop_lag_max_us) {
            destination->set_event_loop_lag_max_us(*source.event_loop_lag_max_us);
        }
        if (source.oldest_completed_unix_ms) {
            destination->set_oldest_completed_unix_ms(*source.oldest_completed_unix_ms);
        }
        if (source.newest_completed_unix_ms) {
            destination->set_newest_completed_unix_ms(*source.newest_completed_unix_ms);
        }
    }
    return response;
}

Response dispatch_request(const Request& request, const QuotaManager& quotas, const CommandMetrics& command_metrics)
{
    switch (static_cast<CtlMethod>(request.method())) {
    case CtlMethod::GET_QUOTA_STATE:
        if (!request.payload().empty()) {
            return make_error(StatusCode::INVALID_ARGUMENT, "GetQuotaState request must be empty");
        }
        return make_ok(make_quota_state_response(quotas.snapshot()));
    case CtlMethod::GET_COMMAND_LATENCY:
        if (!request.payload().empty()) {
            return make_error(StatusCode::INVALID_ARGUMENT, "GetCommandLatency request must be empty");
        }
        return make_ok(make_command_latency_response(command_metrics.snapshot()));
    default:
        return make_error(StatusCode::INVALID_ARGUMENT, "unknown method");
    }
}

} // anonymous namespace

ManagementServer::ManagementServer(UnixSocketEndpoint endpoint, QuotaManager& quotas,
    const CommandMetrics& command_metrics, PeerCredentialReader credential_reader)
    : endpoint_ { std::move(endpoint) }
    , quotas_ { quotas }
    , command_metrics_ { command_metrics }
    , credential_reader_ { std::move(credential_reader) }
{
    asio::spawn(endpoint_.acceptor().get_executor(), [this](Yield yield) { accept_loop(yield); });
}

void ManagementServer::shutdown()
{
    boost::system::error_code error;
    endpoint_.acceptor().close(error);
    for (auto* socket : active_sockets_) {
        socket->cancel(error);
    }
}

void ManagementServer::accept_loop(Yield yield)
{
    boost::system::error_code error;
    for (;;) {
        Socket socket(endpoint_.acceptor().get_executor());
        endpoint_.acceptor().async_accept(socket, yield[error]);
        if (error) {
            switch (classify_accept_error(error)) {
            case AcceptErrorAction::Stop:
                return;
            case AcceptErrorAction::Backoff: {
                log_error("management accept_loop: {} (backing off {}ms)", error.message(), ACCEPT_BACKOFF.count());
                asio::steady_timer timer(endpoint_.acceptor().get_executor());
                timer.expires_after(ACCEPT_BACKOFF);
                boost::system::error_code timer_error;
                timer.async_wait(yield[timer_error]);
                if (timer_error == asio::error::operation_aborted) {
                    return;
                }
                continue;
            }
            case AcceptErrorAction::Continue:
                log_error("management accept_loop: {}", error.message());
                continue;
            }
        }

        auto peer = credential_reader_(socket.native_handle());
        if (!peer) {
            log_error("management accept_loop: failed to read peer credentials");
            socket.close(error);
            continue;
        }

        asio::spawn(endpoint_.acceptor().get_executor(),
            [this, socket = std::move(socket), peer = *peer](
                Yield child_yield) mutable { client_loop(std::move(socket), peer, child_yield); });
    }
}

void ManagementServer::client_loop(Socket socket, PeerCredentials peer, Yield yield)
{
    active_sockets_.push_back(&socket);

    std::string payload;
    if (!read_frame(socket, payload, yield)) {
        std::erase(active_sockets_, &socket);
        return;
    }

    Request request;
    Response response;
    uint32_t method = 0;
    if (!request.ParseFromString(payload)) {
        response = make_error(StatusCode::INVALID_ARGUMENT, "failed to parse request");
    } else {
        method = request.method();
        response = dispatch_request(request, quotas_, command_metrics_);
    }

    const auto wrote_response = write_frame(socket, response, yield);
    log_info("management request: pid {} uid {} gid {} method {} status {}{}", peer.pid, peer.uid, peer.gid, method,
        static_cast<int>(response.status_code()), wrote_response ? "" : " write failed");

    boost::system::error_code error;
    socket.close(error);
    std::erase(active_sockets_, &socket);
}

bool ManagementServer::read_frame(Socket& socket, std::string& payload, Yield yield)
{
    boost::system::error_code error;
    storage_lender::wire::FrameHeader header { };
    asio::async_read(socket, asio::buffer(header.data(), header.size()), yield[error]);
    if (error) {
        return false;
    }

    uint32_t length = 0;
    if (!storage_lender::wire::decode_frame_header(header, length)) {
        log_warn("management read_frame: oversized frame ({} bytes)", length);
        return false;
    }
    payload.resize(length);
    if (length > 0) {
        asio::async_read(socket, asio::buffer(payload.data(), length), yield[error]);
        if (error) {
            return false;
        }
    }
    return true;
}

bool ManagementServer::write_frame(Socket& socket, const Response& response, Yield yield)
{
    const auto payload = response.SerializeAsString();
    storage_lender::wire::FrameHeader header { };
    if (!storage_lender::wire::encode_frame_header(payload.size(), header)) {
        log_error("management write_frame: oversized frame ({} bytes)", payload.size());
        return false;
    }

    boost::system::error_code error;
    const std::array<asio::const_buffer, 2> buffers {
        asio::buffer(header.data(), header.size()),
        asio::buffer(payload),
    };
    asio::async_write(socket, buffers, yield[error]);
    return !error;
}
