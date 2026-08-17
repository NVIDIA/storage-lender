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

#include "buffer_manager.hpp"
#include "command_metrics.hpp"
#include "config_loader.hpp"
#include "device_manager.hpp"
#include "management_server.hpp"
#include "nvme_backend.hpp"
#include "peer_credential_reader.hpp"
#include "queue_manager.hpp"
#include "quota_manager.hpp"
#include "unix_socket_endpoint.hpp"
#include "wire.pb.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <sys/types.h>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/spawn.hpp>
#include <format>
#include <string>

class StorageLenderServer {
public:
    StorageLenderServer(boost::asio::io_context& ioc, UnixSocketEndpoint api_endpoint, UnixSocketEndpoint ctl_endpoint,
        NvmeBackend& backend, DeviceManager& dm, ServerConfig config, std::string config_path,
        PeerCredentialReader credential_reader = { });

    void shutdown();
    void request_reload();
    UsageSnapshot usage_snapshot() const;

private:
    using Socket = boost::asio::local::stream_protocol::socket;
    using Yield = boost::asio::yield_context;
    using Request = nvidia::storage_lender::wire::v1::Request;
    using Response = nvidia::storage_lender::wire::v1::Response;

    struct DeviceHandle {
        uint32_t id;
        QuotaLease quota;
    };

    struct ClientSession {
        ClientSession(Socket socket, NvmeBackend& backend, PeerCredentials peer, PrincipalId principal_id,
            QuotaLease session_lease);
        ~ClientSession();

        std::optional<QuotaLease> quota;
        Socket sock;
        PeerCredentials credentials;
        PrincipalId principal;
        std::vector<DeviceHandle> devices;
        BufferManager buf_manager;
        QueueManager queue_manager;
        SleepFn sleep_fn;

        friend std::string format_as(const ClientSession& session)
        {
            return std::format(
                "[pid={} fd={}]", session.credentials.pid, const_cast<Socket&>(session.sock).native_handle());
        }
    };

    boost::asio::io_context& ioc_;
    std::string config_path_;
    SocketConfig api_socket_config_;
    SocketConfig ctl_socket_config_;
    DevicePolicy device_policy_config_;
    LoggingConfig logging_config_;
    bool reload_pending_ { false };
    QuotaManager quota_manager_;
    PrincipalResolver principal_resolver_;
    PeerCredentialReader credential_reader_;
    UnixSocketEndpoint api_endpoint_;
    CommandMetrics command_metrics_;
    ManagementServer management_server_;
    std::vector<Socket*> active_sockets_;
    NvmeBackend& backend_;
    DeviceManager& dev_manager_;

    void accept_loop(Yield yield);
    void client_loop(Socket sock, PeerCredentials peer, PrincipalId principal, QuotaLease session_quota, Yield yield);
    void cleanup_session(ClientSession& session);
    void reload_config();

    void log_quota_denial(const PeerCredentials& peer, const PrincipalId& principal, std::string_view operation,
        const QuotaExceeded& denial);
    void log_quota_accounting_overflow(
        const PeerCredentials& peer, std::string_view operation, const QuotaAccountingOverflow& overflow);
    Response quota_error_response(const ClientSession& session, std::string_view operation,
        const QuotaAcquireError& error, std::string_view denial_message);

    bool read_frame(ClientSession& session, std::string& payload, Yield yield);
    bool write_frame(ClientSession& session, const Response& resp, Yield yield);

    Response dispatch(const Request& req, ClientSession& session, const SleepFn& sleep_fn, Yield yield);

    Response handle_transfer_fd(ClientSession& session, Yield yield);
    Response handle_map_buffer(const std::string& payload, ClientSession& session);
    Response handle_unmap_buffer(const std::string& payload, ClientSession& session);
    Response handle_open_device(const std::string& payload, ClientSession& session);
    Response handle_close_device(const std::string& payload, ClientSession& session);
    Response handle_get_device_info(const std::string& payload, const ClientSession& session);
    Response handle_create_cq(const std::string& payload, ClientSession& session, const SleepFn& sleep_fn);
    Response handle_delete_cq(const std::string& payload, ClientSession& session, const SleepFn& sleep_fn);
    Response handle_create_sq(const std::string& payload, ClientSession& session, const SleepFn& sleep_fn);
    Response handle_delete_sq(const std::string& payload, ClientSession& session, const SleepFn& sleep_fn);
};
