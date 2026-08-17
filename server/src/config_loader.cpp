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

#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>

#include "config_loader.hpp"
#include "detail/posix_raii.hpp"
#include "logging.hpp"
#include "pci_address.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

struct LimitField {
    std::string_view name;
    bool bytes;
    QuotaLimit QuotaLimits::* member;
};

constexpr uint32_t MIN_IO_QUEUES = 1;
constexpr uint32_t MAX_IO_QUEUES = 65534;
constexpr uint32_t MIN_ADMIN_TIMEOUT_MS = 10000;

constexpr std::array LIMIT_FIELDS {
    LimitField { "sessions", false, &QuotaLimits::sessions },
    LimitField { "transferred_fds", false, &QuotaLimits::transferred_fds },
    LimitField { "mapped_buffers", false, &QuotaLimits::mapped_buffers },
    LimitField { "max_buffer_bytes", true, &QuotaLimits::max_buffer_bytes },
    LimitField { "mapped_bytes", true, &QuotaLimits::mapped_bytes },
    LimitField { "device_handles", false, &QuotaLimits::device_handles },
    LimitField { "completion_queues", false, &QuotaLimits::completion_queues },
    LimitField { "submission_queues", false, &QuotaLimits::submission_queues },
};

ConfigError make_error(
    std::string_view source, std::string_view path, std::string message, ConfigErrorKind kind = ConfigErrorKind::CONFIG)
{
    return { std::string { source }, std::string { path }, std::move(message), kind };
}

std::string child_path(std::string_view parent, std::string_view child)
{
    if (parent.empty()) {
        return std::string { child };
    }
    return std::string { parent } + "." + std::string { child };
}

template <std::size_t N>
std::expected<void, ConfigError> validate_keys(const toml::table& table, const std::array<std::string_view, N>& allowed,
    std::string_view source, std::string_view path)
{
    for (const auto& [key, node] : table) {
        static_cast<void>(node);
        auto name = key.str();
        if (std::ranges::find(allowed, name) == allowed.end()) {
            auto field_path = child_path(path, name);
            return std::unexpected(make_error(source, field_path, "unknown configuration field '" + field_path + "'"));
        }
    }
    return { };
}

QuotaLimits default_global_limits()
{
    return QuotaLimits {
        .sessions = 64,
        .transferred_fds = 256,
        .mapped_buffers = 128,
        .max_buffer_bytes = 8ULL * 1024 * 1024 * 1024,
        .mapped_bytes = 64ULL * 1024 * 1024 * 1024,
        .device_handles = 64,
        .completion_queues = 512,
        .submission_queues = 512,
    };
}

QuotaLimits default_principal_limits()
{
    return QuotaLimits {
        .sessions = 2,
        .transferred_fds = 8,
        .mapped_buffers = 8,
        .max_buffer_bytes = 1ULL * 1024 * 1024 * 1024,
        .mapped_bytes = 2ULL * 1024 * 1024 * 1024,
        .device_handles = 2,
        .completion_queues = 16,
        .submission_queues = 16,
    };
}

ServerConfig default_server_config()
{
    return ServerConfig {
        .api_socket = { "/run/storage-lender/api.sock", "root", "storage-lender", 0660 },
        .ctl_socket = { "/run/storage-lender/ctl.sock", "root", "root", 0600 },
        .logging = { .level = logging::default_level() },
        .device_policy = {
            .default_options = {
                .num_io_queues = MAX_IO_QUEUES,
                .admin_command_timeout = std::chrono::milliseconds { MIN_ADMIN_TIMEOUT_MS },
            },
            .overrides = { },
        },
        .quota_policy = {
            .mode = QuotaMode::ENFORCED,
            .global = default_global_limits(),
            .default_principal = default_principal_limits(),
            .principals = { },
        },
        .principal_resolver = PrincipalResolver { { }, { } },
    };
}

std::expected<SocketConfig, ConfigError> parse_socket_config(
    const toml::table& socket, std::string_view socket_path, std::string_view source_name, SocketConfig config)
{
    constexpr std::array<std::string_view, 4> SOCKET_KEYS { "path", "owner", "group", "mode" };
    if (auto keys = validate_keys(socket, SOCKET_KEYS, source_name, socket_path); !keys) {
        return std::unexpected(keys.error());
    }

    if (const auto* node = socket.get("path"); node != nullptr) {
        const auto value = node->value_exact<std::string_view>();
        if (!value) {
            return std::unexpected(make_error(source_name, child_path(socket_path, "path"), "expected a string"));
        }
        if (auto error = socket_path_validation_error(*value)) {
            return std::unexpected(make_error(source_name, child_path(socket_path, "path"), std::move(*error)));
        }
        config.path = *value;
    }

    if (const auto* node = socket.get("owner"); node != nullptr) {
        const auto value = node->value_exact<std::string_view>();
        if (!value) {
            return std::unexpected(make_error(source_name, child_path(socket_path, "owner"), "expected a string"));
        }
        if (value->empty()) {
            return std::unexpected(
                make_error(source_name, child_path(socket_path, "owner"), "owner must not be empty"));
        }
        config.owner = *value;
    }

    if (const auto* node = socket.get("group"); node != nullptr) {
        const auto value = node->value_exact<std::string_view>();
        if (!value) {
            return std::unexpected(make_error(source_name, child_path(socket_path, "group"), "expected a string"));
        }
        if (value->empty()) {
            return std::unexpected(
                make_error(source_name, child_path(socket_path, "group"), "group must not be empty"));
        }
        config.group = *value;
    }

    if (const auto* node = socket.get("mode"); node != nullptr) {
        const auto value = node->value_exact<int64_t>();
        if (!value) {
            return std::unexpected(make_error(source_name, child_path(socket_path, "mode"), "expected an integer"));
        }
        if (*value < 0 || *value > 0777 || static_cast<uint64_t>(*value) > std::numeric_limits<mode_t>::max()) {
            return std::unexpected(
                make_error(source_name, child_path(socket_path, "mode"), "mode must be between 0o000 and 0o777"));
        }
        const auto mode = static_cast<mode_t>(*value);
        if (!is_allowed_socket_mode(mode)) {
            return std::unexpected(make_error(source_name, child_path(socket_path, "mode"),
                "mode must grant owner read/write and may only grant group read/write"));
        }
        config.mode = mode;
    }

    return config;
}

std::expected<uint32_t, ConfigError> parse_device_integer(
    const toml::node& node, uint32_t minimum, uint32_t maximum, std::string_view source, std::string_view path)
{
    const auto value = node.value_exact<int64_t>();
    if (!value) {
        return std::unexpected(make_error(source, path, "expected an integer"));
    }
    if (*value < minimum || static_cast<uint64_t>(*value) > maximum) {
        return std::unexpected(make_error(
            source, path, "value must be between " + std::to_string(minimum) + " and " + std::to_string(maximum)));
    }
    return static_cast<uint32_t>(*value);
}

std::expected<NvmeConnectOptions, ConfigError> parse_device_options(
    const toml::table& table, std::string_view source, std::string_view path, NvmeConnectOptions options)
{
    constexpr std::array<std::string_view, 2> DEVICE_KEYS { "num_io_queues", "admin_command_timeout_ms" };
    if (auto keys = validate_keys(table, DEVICE_KEYS, source, path); !keys) {
        return std::unexpected(keys.error());
    }

    const auto* queue_node = table.get("num_io_queues");
    if (queue_node != nullptr) {
        auto queues = parse_device_integer(
            *queue_node, MIN_IO_QUEUES, MAX_IO_QUEUES, source, child_path(path, "num_io_queues"));
        if (!queues) {
            return std::unexpected(queues.error());
        }
        options.num_io_queues = *queues;
    }

    const auto* timeout_node = table.get("admin_command_timeout_ms");
    if (timeout_node != nullptr) {
        auto timeout = parse_device_integer(*timeout_node, MIN_ADMIN_TIMEOUT_MS, std::numeric_limits<uint32_t>::max(),
            source, child_path(path, "admin_command_timeout_ms"));
        if (!timeout) {
            return std::unexpected(timeout.error());
        }
        options.admin_command_timeout = std::chrono::milliseconds { *timeout };
    }
    return options;
}

std::expected<DevicePolicy, ConfigError> parse_device_policy(
    const toml::table& root, std::string_view source, DevicePolicy policy)
{
    const auto* devices = root.get("devices")->as_table();
    if (devices == nullptr) {
        return std::unexpected(make_error(source, "devices", "expected a table"));
    }
    if (const auto* default_node = devices->get("default"); default_node != nullptr) {
        const auto* default_table = default_node->as_table();
        if (default_table == nullptr) {
            return std::unexpected(make_error(source, "devices.default", "expected a table"));
        }
        auto defaults = parse_device_options(*default_table, source, "devices.default", policy.default_options);
        if (!defaults) {
            return std::unexpected(defaults.error());
        }
        policy.default_options = *defaults;
    }

    policy.overrides.clear();
    for (const auto& [key, node] : *devices) {
        const auto selector = std::string { key.str() };
        if (selector == "default") {
            continue;
        }
        const auto path = "devices." + selector;
        const auto* table = node.as_table();
        if (table == nullptr) {
            return std::unexpected(make_error(source, path, "expected a device policy table"));
        }
        auto canonical = canonicalize_pci_bdf(selector);
        if (!canonical) {
            return std::unexpected(make_error(source, path, "device selector must be a PCI BDF"));
        }
        auto options = parse_device_options(*table, source, path, policy.default_options);
        if (!options) {
            return std::unexpected(options.error());
        }
        if (!policy.overrides.emplace(*canonical, *options).second) {
            return std::unexpected(
                make_error(source, path, "device selector duplicates canonical BDF '" + *canonical + "'"));
        }
    }
    return policy;
}

std::expected<LoggingConfig, ConfigError> parse_logging(const toml::table& root, std::string_view source)
{
    const auto* node = root.get("logging");
    if (node == nullptr) {
        return LoggingConfig { .level = logging::default_level() };
    }

    const auto* table = node->as_table();
    if (table == nullptr) {
        return std::unexpected(make_error(source, "logging", "expected a table"));
    }

    constexpr std::array<std::string_view, 1> LOGGING_KEYS { "level" };
    if (auto keys = validate_keys(*table, LOGGING_KEYS, source, "logging"); !keys) {
        return std::unexpected(keys.error());
    }

    const auto* level_node = table->get("level");
    if (level_node == nullptr) {
        return LoggingConfig { .level = logging::default_level() };
    }
    const auto text = level_node->value_exact<std::string_view>();
    if (!text) {
        return std::unexpected(make_error(source, "logging.level", "expected a string"));
    }

    struct NamedLevel {
        std::string_view name;
        LogLevel level;
    };
    constexpr std::array LEVELS {
        NamedLevel { "trace", LogLevel::TRACE },
        NamedLevel { "debug", LogLevel::DEBUG },
        NamedLevel { "info", LogLevel::INFO },
        NamedLevel { "warn", LogLevel::WARN },
        NamedLevel { "error", LogLevel::ERROR },
        NamedLevel { "critical", LogLevel::CRITICAL },
        NamedLevel { "off", LogLevel::OFF },
    };
    const auto selected = std::ranges::find(LEVELS, *text, &NamedLevel::name);
    if (selected == LEVELS.end()) {
        return std::unexpected(make_error(
            source, "logging.level", "level must be exactly trace, debug, info, warn, error, critical, or off"));
    }
    if (!logging::is_available(selected->level)) {
        return std::unexpected(make_error(source, "logging.level",
            "level \"" + std::string { *text } + "\" is unavailable; this binary contains "
                + std::string { log_level_name(logging::compiled_minimum_level()) } + " and higher"));
    }
    return LoggingConfig { .level = selected->level };
}

std::expected<QuotaLimit, ConfigError> parse_limit(
    const toml::node& node, bool bytes, std::string_view source, std::string_view path)
{
    if (auto integer = node.value_exact<int64_t>()) {
        if (*integer < 0) {
            return std::unexpected(make_error(source, path, "limit must be non-negative"));
        }
        return QuotaLimit { static_cast<uint64_t>(*integer) };
    }

    auto text = node.value_exact<std::string_view>();
    if (!text) {
        return std::unexpected(make_error(source, path,
            bytes ? "expected a non-negative integer, size string, or \"unlimited\""
                  : "expected a non-negative integer or \"unlimited\""));
    }
    if (*text == "unlimited") {
        return QuotaLimit { std::nullopt };
    }
    if (!bytes) {
        return std::unexpected(make_error(source, path, "count limit string must be exactly \"unlimited\""));
    }

    struct Suffix {
        std::string_view text;
        uint64_t multiplier;
    };
    constexpr std::array SUFFIXES {
        Suffix { "KiB", 1024 },
        Suffix { "MiB", 1024 * 1024 },
        Suffix { "GiB", 1024ULL * 1024 * 1024 },
        Suffix { "B", 1 },
    };

    std::string_view digits;
    uint64_t multiplier = 0;
    for (const auto& suffix : SUFFIXES) {
        if (text->ends_with(suffix.text)) {
            digits = text->substr(0, text->size() - suffix.text.size());
            multiplier = suffix.multiplier;
            break;
        }
    }
    if (multiplier == 0 || digits.empty()) {
        return std::unexpected(make_error(source, path, "size must be digits followed by B, KiB, MiB, or GiB"));
    }

    uint64_t value = 0;
    const auto conversion = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (conversion.ec != std::errc { } || conversion.ptr != digits.data() + digits.size()) {
        return std::unexpected(
            make_error(source, path, "size must contain unsigned decimal digits without whitespace"));
    }
    if (value > std::numeric_limits<uint64_t>::max() / multiplier) {
        return std::unexpected(make_error(source, path, "size exceeds the uint64 byte range"));
    }
    return QuotaLimit { value * multiplier };
}

std::expected<QuotaLimits, ConfigError> parse_limits(
    const toml::table& table, std::string_view source, std::string_view path, QuotaLimits limits)
{
    constexpr std::array<std::string_view, LIMIT_FIELDS.size()> ALLOWED {
        "sessions",
        "transferred_fds",
        "mapped_buffers",
        "max_buffer_bytes",
        "mapped_bytes",
        "device_handles",
        "completion_queues",
        "submission_queues",
    };
    if (auto keys = validate_keys(table, ALLOWED, source, path); !keys) {
        return std::unexpected(keys.error());
    }

    for (const auto& field : LIMIT_FIELDS) {
        auto field_path = child_path(path, field.name);
        const auto* node = table.get(field.name);
        if (node == nullptr) {
            continue;
        }
        auto limit = parse_limit(*node, field.bytes, source, field_path);
        if (!limit) {
            return std::unexpected(limit.error());
        }
        limits.*(field.member) = *limit;
    }
    return limits;
}

template <typename Identity>
std::expected<std::size_t, ConfigError> parse_mappings(const toml::table& principal, std::string_view key,
    std::string_view principal_name, std::string_view source, std::unordered_map<Identity, PrincipalId>& mappings)
{
    const auto* node = principal.get(key);
    if (node == nullptr) {
        return 0;
    }
    auto path = "principals." + std::string { principal_name } + "." + std::string { key };
    const auto* array = node->as_array();
    if (array == nullptr) {
        return std::unexpected(make_error(source, path, "expected an array of integers"));
    }

    for (std::size_t index = 0; index < array->size(); ++index) {
        const auto value = (*array)[index].value_exact<int64_t>();
        auto element_path = path + "[" + std::to_string(index) + "]";
        if (!value || *value < 0 || static_cast<uint64_t>(*value) > std::numeric_limits<Identity>::max()) {
            return std::unexpected(
                make_error(source, element_path, "identity must be a non-negative integer in range"));
        }

        auto identity = static_cast<Identity>(*value);
        auto [existing, inserted] = mappings.emplace(identity, principal_name);
        if (!inserted) {
            return std::unexpected(make_error(source, path,
                "identity " + std::to_string(static_cast<uint64_t>(identity)) + " is already mapped to principal '"
                    + existing->second + "'"));
        }
    }
    return array->size();
}

struct PrincipalTable {
    std::string name;
    const toml::table* table;
    uint32_t source_line;
};

std::expected<std::vector<PrincipalTable>, ConfigError> collect_principals(
    const toml::table& root, std::string_view source)
{
    const auto* node = root.get("principals");
    if (node == nullptr) {
        return std::vector<PrincipalTable> { };
    }
    const auto* principals = node->as_table();
    if (principals == nullptr) {
        return std::unexpected(make_error(source, "principals", "expected a table"));
    }

    std::vector<PrincipalTable> result;
    result.reserve(principals->size());
    for (const auto& [key, value] : *principals) {
        auto name = std::string { key.str() };
        if (name == "default") {
            return std::unexpected(
                make_error(source, "principals.default", "principal name 'default' is reserved for unmatched peers"));
        }
        const auto* table = value.as_table();
        if (table == nullptr) {
            return std::unexpected(make_error(source, "principals." + name, "expected a named principal table"));
        }
        result.push_back({ std::move(name), table, value.source().begin.line });
    }
    std::ranges::sort(result, { }, &PrincipalTable::source_line);
    return result;
}

}

std::expected<ServerConfig, ConfigError> ConfigLoader::load(const std::string& path)
{
    storage_lender::server_detail::UniqueFd input { ::open(path.c_str(), O_RDONLY | O_CLOEXEC) };
    if (!input) {
        const auto error = errno;
        const auto kind = error == ENOENT ? ConfigErrorKind::FILE_NOT_FOUND : ConfigErrorKind::FILE_IO;
        return std::unexpected(
            make_error(path, "$file", "unable to open configuration file: " + std::string { ::strerror(error) }, kind));
    }

    std::string document;
    std::array<char, 8192> buffer;
    while (true) {
        const auto count = ::read(input.get(), buffer.data(), buffer.size());
        if (count > 0) {
            document.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            break;
        }
        const auto error = errno;
        if (error == EINTR) {
            continue;
        }
        return std::unexpected(make_error(path, "$file",
            "unable to read configuration file: " + std::string { ::strerror(error) }, ConfigErrorKind::FILE_IO));
    }
    return parse(document, path);
}

std::expected<StartupConfig, ConfigError> ConfigLoader::load_for_startup(const std::string& path)
{
    auto config = load(path);
    if (config) {
        return StartupConfig { .config = std::move(*config), .used_builtin_defaults = false };
    }
    if (config.error().kind != ConfigErrorKind::FILE_NOT_FOUND) {
        return std::unexpected(config.error());
    }
    return StartupConfig { .config = default_server_config(), .used_builtin_defaults = true };
}

std::expected<ServerConfig, ConfigError> ConfigLoader::parse(std::string_view document, std::string_view source_name)
{
    auto parsed = toml::parse(document, source_name);
    if (!parsed) {
        const auto& parse_error = parsed.error();
        auto message = std::string { parse_error.description() } + " at line "
            + std::to_string(parse_error.source().begin.line) + ", column "
            + std::to_string(parse_error.source().begin.column);
        return std::unexpected(make_error(source_name, "$syntax", std::move(message)));
    }
    auto root = std::move(parsed).table();
    auto config = default_server_config();

    constexpr std::array<std::string_view, 6> ROOT_KEYS { "schema_version", "logging", "sockets", "devices", "quotas",
        "principals" };
    if (auto keys = validate_keys(root, ROOT_KEYS, source_name, ""); !keys) {
        return std::unexpected(keys.error());
    }

    const auto* version_node = root.get("schema_version");
    if (version_node != nullptr) {
        const auto version = version_node->value_exact<int64_t>();
        if (!version || *version != 1) {
            return std::unexpected(make_error(source_name, "schema_version", "schema_version must be integer 1"));
        }
    }

    auto logging_config = parse_logging(root, source_name);
    if (!logging_config) {
        return std::unexpected(logging_config.error());
    }

    auto device_policy = std::expected<DevicePolicy, ConfigError> { config.device_policy };
    if (root.contains("devices")) {
        device_policy = parse_device_policy(root, source_name, config.device_policy);
        if (!device_policy) {
            return std::unexpected(device_policy.error());
        }
    }

    auto api_socket = std::expected<SocketConfig, ConfigError> { config.api_socket };
    auto ctl_socket = std::expected<SocketConfig, ConfigError> { config.ctl_socket };
    if (const auto* sockets_node = root.get("sockets"); sockets_node != nullptr) {
        const auto* sockets = sockets_node->as_table();
        if (sockets == nullptr) {
            return std::unexpected(make_error(source_name, "sockets", "expected a table"));
        }
        constexpr std::array<std::string_view, 2> SOCKET_KEYS { "api", "ctl" };
        if (auto keys = validate_keys(*sockets, SOCKET_KEYS, source_name, "sockets"); !keys) {
            return std::unexpected(keys.error());
        }
        if (const auto* node = sockets->get("api"); node != nullptr) {
            const auto* table = node->as_table();
            if (table == nullptr) {
                return std::unexpected(make_error(source_name, "sockets.api", "expected a table"));
            }
            api_socket = parse_socket_config(*table, "sockets.api", source_name, std::move(*api_socket));
            if (!api_socket) {
                return std::unexpected(api_socket.error());
            }
        }
        if (const auto* node = sockets->get("ctl"); node != nullptr) {
            const auto* table = node->as_table();
            if (table == nullptr) {
                return std::unexpected(make_error(source_name, "sockets.ctl", "expected a table"));
            }
            ctl_socket = parse_socket_config(*table, "sockets.ctl", source_name, std::move(*ctl_socket));
            if (!ctl_socket) {
                return std::unexpected(ctl_socket.error());
            }
        }
    }

    auto policy = config.quota_policy;
    if (const auto* quotas_node = root.get("quotas"); quotas_node != nullptr) {
        const auto* quotas = quotas_node->as_table();
        if (quotas == nullptr) {
            return std::unexpected(make_error(source_name, "quotas", "expected a table"));
        }
        constexpr std::array<std::string_view, 3> QUOTA_KEYS { "mode", "global", "default" };
        if (auto keys = validate_keys(*quotas, QUOTA_KEYS, source_name, "quotas"); !keys) {
            return std::unexpected(keys.error());
        }

        if (const auto* mode_node = quotas->get("mode"); mode_node != nullptr) {
            const auto mode_text = mode_node->value_exact<std::string_view>();
            if (!mode_text || (*mode_text != "enforced" && *mode_text != "unlimited")) {
                return std::unexpected(
                    make_error(source_name, "quotas.mode", "mode must be exactly \"enforced\" or \"unlimited\""));
            }
            policy.mode = *mode_text == "enforced" ? QuotaMode::ENFORCED : QuotaMode::UNLIMITED;
        }

        if (policy.mode == QuotaMode::UNLIMITED) {
            for (auto key : { std::string_view { "global" }, std::string_view { "default" } }) {
                if (quotas->contains(key)) {
                    auto path = child_path("quotas", key);
                    return std::unexpected(
                        make_error(source_name, path, "limit tables are not allowed in unlimited mode"));
                }
            }
            policy.global = { };
            policy.default_principal = { };
            policy.principals.clear();
        } else {
            if (const auto* node = quotas->get("global"); node != nullptr) {
                const auto* table = node->as_table();
                if (table == nullptr) {
                    return std::unexpected(make_error(source_name, "quotas.global", "expected a table"));
                }
                auto global = parse_limits(*table, source_name, "quotas.global", policy.global);
                if (!global) {
                    return std::unexpected(global.error());
                }
                policy.global = *global;
            }
            if (const auto* node = quotas->get("default"); node != nullptr) {
                const auto* table = node->as_table();
                if (table == nullptr) {
                    return std::unexpected(make_error(source_name, "quotas.default", "expected a table"));
                }
                auto defaults = parse_limits(*table, source_name, "quotas.default", policy.default_principal);
                if (!defaults) {
                    return std::unexpected(defaults.error());
                }
                policy.default_principal = *defaults;
            }
        }
    }

    auto principals = collect_principals(root, source_name);
    if (!principals) {
        return std::unexpected(principals.error());
    }

    PrincipalResolver::UidMappings uids;
    PrincipalResolver::GidMappings gids;
    constexpr std::array<std::string_view, 3> PRINCIPAL_KEYS { "uids", "gids", "quotas" };
    for (const auto& principal : *principals) {
        auto principal_path = "principals." + principal.name;
        if (auto keys = validate_keys(*principal.table, PRINCIPAL_KEYS, source_name, principal_path); !keys) {
            return std::unexpected(keys.error());
        }

        const auto* quota_node = principal.table->get("quotas");
        QuotaLimits limits;
        if (policy.mode == QuotaMode::UNLIMITED) {
            if (quota_node != nullptr) {
                auto path = principal_path + ".quotas";
                return std::unexpected(
                    make_error(source_name, path, "principal limit tables are not allowed in unlimited mode"));
            }
        } else if (quota_node == nullptr) {
            limits = policy.default_principal;
        } else {
            const auto* quota_table = quota_node->as_table();
            auto path = principal_path + ".quotas";
            if (quota_table == nullptr) {
                return std::unexpected(make_error(source_name, path, "expected a table"));
            }
            auto parsed_limits = parse_limits(*quota_table, source_name, path, policy.default_principal);
            if (!parsed_limits) {
                return std::unexpected(parsed_limits.error());
            }
            limits = *parsed_limits;
        }

        auto uid_count = parse_mappings(*principal.table, "uids", principal.name, source_name, uids);
        if (!uid_count) {
            return std::unexpected(uid_count.error());
        }
        auto gid_count = parse_mappings(*principal.table, "gids", principal.name, source_name, gids);
        if (!gid_count) {
            return std::unexpected(gid_count.error());
        }
        if (*uid_count + *gid_count == 0) {
            return std::unexpected(
                make_error(source_name, principal_path, "principal requires at least one UID or GID mapping"));
        }
        policy.principals.emplace(principal.name, std::move(limits));
    }

    config.api_socket = std::move(*api_socket);
    config.ctl_socket = std::move(*ctl_socket);
    config.logging = *logging_config;
    config.device_policy = std::move(*device_policy);
    config.quota_policy = std::move(policy);
    config.principal_resolver = PrincipalResolver { std::move(uids), std::move(gids) };
    return config;
}
