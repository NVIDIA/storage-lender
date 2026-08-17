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

#include "config_loader.hpp"
#include "device_manager.hpp"
#include "logging.hpp"
#include "server.hpp"
#include "server_options.hpp"
#include "spdk_backend.hpp"
#include "unix_socket_endpoint.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <csignal>
#include <cstdlib>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

int main(int argc, char** argv)
{
    logging::initialize();

    std::vector<std::string_view> arguments;
    for (int i = 1; i < argc; ++i) {
        arguments.emplace_back(argv[i]);
    }
    auto options = parse_server_options(arguments);
    if (!options) {
        log_error("Invalid command line: {}", options.error());
        return EXIT_FAILURE;
    }

    auto loaded = ConfigLoader::load_for_startup(options->config_path);
    if (!loaded) {
        log_error("Failed to load configuration {} at {}: {}", loaded.error().source, loaded.error().path,
            loaded.error().message);
        return EXIT_FAILURE;
    }
    if (loaded->used_builtin_defaults) {
        log_warn("Configuration file {} does not exist; using built-in defaults", options->config_path);
    }
    auto config = std::move(loaded->config);

    logging::set_level(config.logging.level);

    SpdkBackend backend;
    DeviceManager device_manager { backend, config.device_policy };

    boost::asio::io_context ioc;
    auto api_endpoint = UnixSocketEndpoint::create(ioc, config.api_socket);
    if (!api_endpoint) {
        log_error("Failed to create API socket: {}", api_endpoint.error().message());
        return EXIT_FAILURE;
    }
    auto ctl_endpoint = UnixSocketEndpoint::create(ioc, config.ctl_socket);
    if (!ctl_endpoint) {
        log_error("Failed to create ctl socket: {}", ctl_endpoint.error().message());
        return EXIT_FAILURE;
    }

    StorageLenderServer server { ioc, std::move(*api_endpoint), std::move(*ctl_endpoint), backend, device_manager,
        std::move(config), options->config_path };

    boost::asio::signal_set termination_signals { ioc };
    boost::system::error_code ec;
    termination_signals.add(SIGINT, ec);
    if (ec) {
        log_error("Failed to register SIGINT handler: {}", ec.message());
        return EXIT_FAILURE;
    }
    termination_signals.add(SIGTERM, ec);
    if (ec) {
        log_error("Failed to register SIGTERM handler: {}", ec.message());
        return EXIT_FAILURE;
    }

    boost::asio::signal_set reload_signals { ioc };
    reload_signals.add(SIGHUP, ec);
    if (ec) {
        log_error("Failed to register SIGHUP handler: {}", ec.message());
        return EXIT_FAILURE;
    }

    std::function<void()> wait_for_reload;
    wait_for_reload = [&] {
        reload_signals.async_wait([&](boost::system::error_code signal_ec, int) {
            if (signal_ec) {
                return;
            }
            server.request_reload();
            wait_for_reload();
        });
    };
    wait_for_reload();

    termination_signals.async_wait([&](boost::system::error_code signal_ec, int) {
        if (signal_ec) {
            return;
        }
        boost::system::error_code cancel_ec;
        reload_signals.cancel(cancel_ec);
        server.shutdown();
    });

    ioc.run();

    return 0;
}
