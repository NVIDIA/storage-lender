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

#include "ctl_client.hpp"
#include "options.hpp"
#include "render.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

void print_usage(std::ostream& output)
{
    output << "Usage: storage-lender-ctl [OPTIONS] quota|latency\n"
              "\n"
              "Options:\n"
              "  -s, --socket PATH     Management socket path\n"
              "  -f, --format FORMAT   Output format: human or json\n"
              "  -c, --color MODE      Color mode: auto, always, or never\n"
              "  -h, --help             Show this help\n"
              "  -V, --version          Show version\n";
}

void print_client_error(const storage_lender::ctl::CtlClientError& error)
{
    std::cerr << "storage-lender-ctl: ";
    switch (error.category) {
    case storage_lender::ctl::CtlClientError::IO:
        std::cerr << error.message;
        if (error.system_error != 0) {
            std::cerr << ": " << ::strerror(error.system_error);
        }
        break;
    case storage_lender::ctl::CtlClientError::SERVER_STATUS:
        std::cerr << "server returned status " << static_cast<int>(error.server_status);
        if (!error.message.empty()) {
            std::cerr << ": " << error.message;
        }
        break;
    case storage_lender::ctl::CtlClientError::PROTOCOL:
        std::cerr << error.message;
        break;
    }
    std::cerr << '\n';
}

template <typename Fetch, typename HumanRenderer>
int run_command(const storage_lender::ctl::CtlOptions& options, Fetch fetch, HumanRenderer human_renderer)
{
    auto response = fetch(options.socket_path);
    if (!response) {
        print_client_error(response.error());
        return 1;
    }

    std::expected<std::string, std::string> output = std::unexpected(std::string { "unknown output format" });
    switch (options.format) {
    case storage_lender::ctl::OutputFormat::HUMAN: {
        const auto* term = ::getenv("TERM");
        const auto color = storage_lender::ctl::color_enabled(
            options.color, ::isatty(STDOUT_FILENO) != 0, term == nullptr ? "" : term, ::getenv("NO_COLOR") != nullptr);
        output = human_renderer(*response, color);
        break;
    }
    case storage_lender::ctl::OutputFormat::JSON:
        output = storage_lender::ctl::render_json(*response);
        break;
    }
    if (!output) {
        std::cerr << "storage-lender-ctl: failed to render output: " << output.error() << '\n';
        return 1;
    }
    std::cout << *output;
    std::cout.flush();
    if (!std::cout) {
        std::cerr << "storage-lender-ctl: failed to write output\n";
        return 1;
    }
    return 0;
}

} // anonymous namespace

int main(int argc, char* argv[])
{
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    auto options = storage_lender::ctl::parse_ctl_options(arguments);
    if (!options) {
        std::cerr << "storage-lender-ctl: " << options.error() << '\n';
        print_usage(std::cerr);
        return 2;
    }
    if (options->help) {
        print_usage(std::cout);
        return 0;
    }
    if (options->version) {
        std::cout << "storage-lender-ctl " << STORAGE_LENDER_VERSION << '\n';
        return 0;
    }

    switch (*options->command) {
    case storage_lender::ctl::CtlCommand::QUOTA:
        return run_command(*options, storage_lender::ctl::get_quota_state,
            [](const nvidia::storage_lender::ctl::v1::GetQuotaStateResponse& response, bool color) {
                return storage_lender::ctl::render_human(response, color);
            });
    case storage_lender::ctl::CtlCommand::LATENCY:
        return run_command(*options, storage_lender::ctl::get_command_latency,
            [](const nvidia::storage_lender::ctl::v1::GetCommandLatencyResponse& response, bool) {
                return storage_lender::ctl::render_human(response);
            });
    }
    return 1;
}
