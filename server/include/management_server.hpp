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

#include "command_metrics.hpp"
#include "peer_credential_reader.hpp"
#include "quota_manager.hpp"
#include "unix_socket_endpoint.hpp"
#include "wire.pb.h"

#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/spawn.hpp>

#include <string>
#include <vector>

class ManagementServer {
public:
    ManagementServer(UnixSocketEndpoint endpoint, QuotaManager& quotas, const CommandMetrics& command_metrics,
        PeerCredentialReader credential_reader);

    void shutdown();

private:
    using Socket = boost::asio::local::stream_protocol::socket;
    using Yield = boost::asio::yield_context;

    UnixSocketEndpoint endpoint_;
    QuotaManager& quotas_;
    const CommandMetrics& command_metrics_;
    PeerCredentialReader credential_reader_;
    std::vector<Socket*> active_sockets_;

    void accept_loop(Yield yield);
    void client_loop(Socket socket, PeerCredentials peer, Yield yield);
    bool read_frame(Socket& socket, std::string& payload, Yield yield);
    bool write_frame(Socket& socket, const nvidia::storage_lender::wire::v1::Response& response, Yield yield);
};
