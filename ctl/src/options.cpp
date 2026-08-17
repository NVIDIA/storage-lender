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

#include "options.hpp"

namespace storage_lender::ctl {
namespace {

    std::expected<std::string_view, std::string> next_option_value(
        std::span<const std::string_view> arguments, std::size_t& index, std::string_view option)
    {
        if (++index == arguments.size()) {
            return std::unexpected("missing value for " + std::string { option });
        }
        if (arguments[index].empty()) {
            return std::unexpected("empty value for " + std::string { option });
        }
        return arguments[index];
    }

    std::expected<OutputFormat, std::string> parse_format(std::string_view value)
    {
        if (value == "human") {
            return OutputFormat::HUMAN;
        }
        if (value == "json") {
            return OutputFormat::JSON;
        }
        return std::unexpected("unknown format: " + std::string { value });
    }

    std::expected<ColorMode, std::string> parse_color(std::string_view value)
    {
        if (value == "auto") {
            return ColorMode::AUTO;
        }
        if (value == "always") {
            return ColorMode::ALWAYS;
        }
        if (value == "never") {
            return ColorMode::NEVER;
        }
        return std::unexpected("unknown color mode: " + std::string { value });
    }

} // anonymous namespace

std::expected<CtlOptions, std::string> parse_ctl_options(std::span<const std::string_view> arguments)
{
    CtlOptions options;
    bool socket_seen = false;
    bool format_seen = false;
    bool color_seen = false;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto argument = arguments[index];
        if (argument == "-s" || argument == "--socket") {
            if (socket_seen) {
                return std::unexpected("socket option specified more than once");
            }
            auto value = next_option_value(arguments, index, argument);
            if (!value) {
                return std::unexpected(value.error());
            }
            options.socket_path = *value;
            socket_seen = true;
            continue;
        }
        if (argument == "-f" || argument == "--format") {
            if (format_seen) {
                return std::unexpected("format option specified more than once");
            }
            auto value = next_option_value(arguments, index, argument);
            if (!value) {
                return std::unexpected(value.error());
            }
            auto format = parse_format(*value);
            if (!format) {
                return std::unexpected(format.error());
            }
            options.format = *format;
            format_seen = true;
            continue;
        }
        if (argument == "-c" || argument == "--color") {
            if (color_seen) {
                return std::unexpected("color option specified more than once");
            }
            auto value = next_option_value(arguments, index, argument);
            if (!value) {
                return std::unexpected(value.error());
            }
            auto color = parse_color(*value);
            if (!color) {
                return std::unexpected(color.error());
            }
            options.color = *color;
            color_seen = true;
            continue;
        }
        if (argument == "-h" || argument == "--help") {
            options.help = true;
            continue;
        }
        if (argument == "-V" || argument == "--version") {
            options.version = true;
            continue;
        }
        if (argument.starts_with('-')) {
            return std::unexpected("unknown option: " + std::string { argument });
        }
        if (options.command) {
            return std::unexpected("multiple ctl subcommands specified");
        }
        if (argument == "quota") {
            options.command = CtlCommand::QUOTA;
            continue;
        }
        if (argument == "latency") {
            options.command = CtlCommand::LATENCY;
            continue;
        }
        return std::unexpected("unexpected positional argument: " + std::string { argument });
    }

    if (!options.command && !options.help && !options.version) {
        return std::unexpected("expected quota or latency subcommand");
    }
    return options;
}

} // namespace storage_lender::ctl
