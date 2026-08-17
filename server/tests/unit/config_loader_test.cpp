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
#include "logging.hpp"

#include <gtest/gtest.h>

#include <sys/un.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view COMPLETE_CONFIG = R"(
schema_version = 1

[logging]
level = "info"

[sockets.api]
path = "/run/storage-lender/api.sock"
owner = "root"
group = "storage-lender"
mode = 0o660

[devices.default]
num_io_queues = 65534
admin_command_timeout_ms = 10000

[quotas]
mode = "enforced"

[quotas.global]
sessions = 64
transferred_fds = 256
mapped_buffers = 128
max_buffer_bytes = "8GiB"
mapped_bytes = "64GiB"
device_handles = 32
completion_queues = 512
submission_queues = 512

[quotas.default]
sessions = 2
transferred_fds = 8
mapped_buffers = 4
max_buffer_bytes = "1GiB"
mapped_bytes = "2GiB"
device_handles = 2
completion_queues = 16
submission_queues = 16

[principals.client_app]
uids = [1001]
gids = [2001]

[principals.client_app.quotas]
sessions = 8
max_buffer_bytes = "4GiB"
mapped_bytes = "32GiB"
completion_queues = 256
submission_queues = 256
)";

constexpr std::string_view COMPLETE_CTL_SOCKET_CONFIG = R"(
[sockets.ctl]
path = "/run/storage-lender/ctl.sock"
owner = "root"
group = "root"
mode = 0o600
)";

std::string replace_once(std::string document, std::string_view needle, std::string_view replacement)
{
    auto position = document.find(needle);
    EXPECT_NE(position, std::string::npos) << needle;
    if (position != std::string::npos) {
        document.replace(position, needle.size(), replacement);
    }
    return document;
}

ConfigError parse_error(std::string_view document)
{
    auto result = ConfigLoader::parse(document, "test.toml");
    EXPECT_FALSE(result.has_value());
    if (result.has_value()) {
        return { };
    }
    EXPECT_EQ(result.error().source, "test.toml");
    EXPECT_FALSE(result.error().path.empty());
    EXPECT_FALSE(result.error().message.empty());
    return result.error();
}

void expect_error_at(std::string_view document, std::string_view path)
{
    auto error = parse_error(document);
    EXPECT_EQ(error.path, path);
}

void expect_builtin_defaults(const ServerConfig& config)
{
    EXPECT_EQ(config.logging.level, logging::default_level());
    EXPECT_EQ(config.api_socket, (SocketConfig { "/run/storage-lender/api.sock", "root", "storage-lender", 0660 }));
    EXPECT_EQ(config.ctl_socket, (SocketConfig { "/run/storage-lender/ctl.sock", "root", "root", 0600 }));
    EXPECT_EQ(config.device_policy.default_options.num_io_queues, 65534u);
    EXPECT_EQ(config.device_policy.default_options.admin_command_timeout, std::chrono::milliseconds { 10000 });
    EXPECT_TRUE(config.device_policy.overrides.empty());

    EXPECT_EQ(config.quota_policy.mode, QuotaMode::ENFORCED);
    EXPECT_EQ(config.quota_policy.global.sessions, 64u);
    EXPECT_EQ(config.quota_policy.global.transferred_fds, 256u);
    EXPECT_EQ(config.quota_policy.global.mapped_buffers, 128u);
    EXPECT_EQ(config.quota_policy.global.max_buffer_bytes, 8ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(config.quota_policy.global.mapped_bytes, 64ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(config.quota_policy.global.device_handles, 64u);
    EXPECT_EQ(config.quota_policy.global.completion_queues, 512u);
    EXPECT_EQ(config.quota_policy.global.submission_queues, 512u);

    EXPECT_EQ(config.quota_policy.default_principal.sessions, 2u);
    EXPECT_EQ(config.quota_policy.default_principal.transferred_fds, 8u);
    EXPECT_EQ(config.quota_policy.default_principal.mapped_buffers, 8u);
    EXPECT_EQ(config.quota_policy.default_principal.max_buffer_bytes, 1ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(config.quota_policy.default_principal.mapped_bytes, 2ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(config.quota_policy.default_principal.device_handles, 2u);
    EXPECT_EQ(config.quota_policy.default_principal.completion_queues, 16u);
    EXPECT_EQ(config.quota_policy.default_principal.submission_queues, 16u);
    EXPECT_TRUE(config.quota_policy.principals.empty());
    EXPECT_EQ(config.principal_resolver.resolve(1001, 2001), "default");
}

}

TEST(ConfigLoaderTest, EmptyAndCommentOnlyDocumentsResolveToBuiltinDefaults)
{
    for (const auto document : { std::string_view { }, std::string_view { "# intentionally empty\n" } }) {
        SCOPED_TRACE(document);
        auto result = ConfigLoader::parse(document, "defaults.toml");
        ASSERT_TRUE(result.has_value()) << result.error().message;
        expect_builtin_defaults(*result);
    }
}

TEST(ConfigLoaderTest, ParsesCompleteEnforcedPolicy)
{
    auto result = ConfigLoader::parse(COMPLETE_CONFIG, "complete.toml");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(result->api_socket.path, "/run/storage-lender/api.sock");
    EXPECT_EQ(result->api_socket.owner, "root");
    EXPECT_EQ(result->api_socket.group, "storage-lender");
    EXPECT_EQ(result->api_socket.mode, 0660);
    EXPECT_EQ(result->ctl_socket.path, "/run/storage-lender/ctl.sock");
    EXPECT_EQ(result->ctl_socket.owner, "root");
    EXPECT_EQ(result->ctl_socket.group, "root");
    EXPECT_EQ(result->ctl_socket.mode, 0600);

    const auto& policy = result->quota_policy;
    EXPECT_EQ(policy.mode, QuotaMode::ENFORCED);
    EXPECT_EQ(policy.global.sessions, 64);
    EXPECT_EQ(policy.global.max_buffer_bytes, 8ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(policy.global.mapped_bytes, 64ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(policy.default_principal.transferred_fds, 8);
    ASSERT_TRUE(policy.principals.contains("client_app"));
    EXPECT_EQ(policy.principals.at("client_app").sessions, 8);
    EXPECT_EQ(policy.principals.at("client_app").mapped_bytes, 32ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(result->principal_resolver.resolve(1001, 99), "client_app");
    EXPECT_EQ(result->principal_resolver.resolve(99, 2001), "client_app");
}

TEST(ConfigLoaderTest, EmptyTablesResolveToBuiltinDefaults)
{
    constexpr std::string_view DOCUMENT = R"(
[logging]
[sockets]
[devices]
[quotas]
[principals]
)";
    auto result = ConfigLoader::parse(DOCUMENT, "empty-tables.toml");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    expect_builtin_defaults(*result);
}

TEST(ConfigLoaderTest, SocketTablesOverlayIndividualFields)
{
    constexpr std::string_view DOCUMENT = R"(
[sockets.api]
mode = 0o600
[sockets.ctl]
path = "/run/storage-lender/admin.sock"
)";
    auto result = ConfigLoader::parse(DOCUMENT, "partial-sockets.toml");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->api_socket.path, "/run/storage-lender/api.sock");
    EXPECT_EQ(result->api_socket.owner, "root");
    EXPECT_EQ(result->api_socket.group, "storage-lender");
    EXPECT_EQ(result->api_socket.mode, 0600);
    EXPECT_EQ(result->ctl_socket.path, "/run/storage-lender/admin.sock");
    EXPECT_EQ(result->ctl_socket.owner, "root");
    EXPECT_EQ(result->ctl_socket.group, "root");
    EXPECT_EQ(result->ctl_socket.mode, 0600);
}

TEST(ConfigLoaderTest, PolicyTablesOverlayIndividualFields)
{
    constexpr std::string_view DOCUMENT = R"(
[logging]
[devices.default]
num_io_queues = 2048
[devices."03:00.0"]
[quotas.global]
sessions = 7
[quotas.default]
mapped_buffers = 3
[principals.camera]
uids = [1001]
[principals.camera.quotas]
device_handles = "unlimited"
)";
    auto result = ConfigLoader::parse(DOCUMENT, "partial-policy.toml");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->logging.level, logging::default_level());
    EXPECT_EQ(result->device_policy.default_options.num_io_queues, 2048u);
    EXPECT_EQ(result->device_policy.default_options.admin_command_timeout, std::chrono::milliseconds { 10000 });
    EXPECT_EQ(result->device_policy.options_for("0000:03:00.0"), result->device_policy.default_options);
    EXPECT_EQ(result->quota_policy.mode, QuotaMode::ENFORCED);
    EXPECT_EQ(result->quota_policy.global.sessions, 7u);
    EXPECT_EQ(result->quota_policy.global.device_handles, 64u);
    EXPECT_EQ(result->quota_policy.default_principal.mapped_buffers, 3u);
    EXPECT_EQ(result->quota_policy.default_principal.sessions, 2u);
    EXPECT_EQ(result->quota_policy.principals.at("camera").device_handles, std::nullopt);
    EXPECT_EQ(result->quota_policy.principals.at("camera").sessions, 2u);
}

TEST(ConfigLoaderTest, ExplicitUnlimitedModeClearsFiniteDefaults)
{
    auto result = ConfigLoader::parse("[quotas]\nmode = \"unlimited\"\n", "unlimited.toml");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->quota_policy.mode, QuotaMode::UNLIMITED);
    EXPECT_EQ(result->quota_policy.global, QuotaLimits { });
    EXPECT_EQ(result->quota_policy.default_principal, QuotaLimits { });
}

TEST(ConfigLoaderTest, ParsesExplicitCtlSocketOverride)
{
    auto result = ConfigLoader::parse(
        std::string { COMPLETE_CONFIG } + std::string { COMPLETE_CTL_SOCKET_CONFIG }, "ctl-override.toml");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(result->ctl_socket.path, "/run/storage-lender/ctl.sock");
    EXPECT_EQ(result->ctl_socket.owner, "root");
    EXPECT_EQ(result->ctl_socket.group, "root");
    EXPECT_EQ(result->ctl_socket.mode, 0600);
}

TEST(ConfigLoaderTest, RejectsInvalidCtlSocketConfiguration)
{
    struct Case {
        std::string document;
        std::string_view path;
    };

    const Case CASES[] = {
        { std::string { COMPLETE_CONFIG } + "\n[sockets.ctl]\nunknown = true\n", "sockets.ctl.unknown" },
        { std::string { COMPLETE_CONFIG }
                + "\n[sockets.ctl]\npath = \"relative/ctl.sock\"\nowner = \"root\"\ngroup = \"root\"\nmode = 0o600\n",
            "sockets.ctl.path" },
        { std::string { COMPLETE_CONFIG }
                + "\n[sockets.ctl]\npath = \"/run/storage-lender/ctl.sock\"\nowner = \"\"\ngroup = \"root\"\nmode = "
                  "0o600\n",
            "sockets.ctl.owner" },
        { std::string { COMPLETE_CONFIG }
                + "\n[sockets.ctl]\npath = \"/run/storage-lender/ctl.sock\"\nowner = \"root\"\ngroup = \"\"\nmode = "
                  "0o600\n",
            "sockets.ctl.group" },
        { std::string { COMPLETE_CONFIG }
                + "\n[sockets.ctl]\npath = \"/run/storage-lender/ctl.sock\"\nowner = \"root\"\ngroup = \"root\"\nmode "
                  "= 0o644\n",
            "sockets.ctl.mode" },
    };

    for (const auto& test_case : CASES) {
        SCOPED_TRACE(test_case.path);
        expect_error_at(test_case.document, test_case.path);
    }
}

TEST(ConfigLoaderTest, ParsesLoggingPolicy)
{
    auto result = ConfigLoader::parse(COMPLETE_CONFIG, "logging.toml");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->logging.level, LogLevel::INFO);
}

TEST(ConfigLoaderTest, ParsesDefaultAndPerDevicePolicy)
{
    auto document = std::string { COMPLETE_CONFIG } + R"(
[devices."03:00.0"]
num_io_queues = 2048

[devices."0001:04:00.0"]
admin_command_timeout_ms = 30000
)";
    auto result = ConfigLoader::parse(document, "devices.toml");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(result->device_policy.default_options.num_io_queues, 65534u);
    EXPECT_EQ(result->device_policy.default_options.admin_command_timeout, std::chrono::milliseconds { 10000 });
    EXPECT_EQ(result->device_policy.options_for("0000:03:00.0").num_io_queues, 2048u);
    EXPECT_EQ(
        result->device_policy.options_for("0000:03:00.0").admin_command_timeout, std::chrono::milliseconds { 10000 });
    EXPECT_EQ(result->device_policy.options_for("0001:04:00.0").num_io_queues, 65534u);
    EXPECT_EQ(
        result->device_policy.options_for("0001:04:00.0").admin_command_timeout, std::chrono::milliseconds { 30000 });
}

TEST(ConfigLoaderTest, AcceptsDevicePolicyNumericBoundaries)
{
    auto document = replace_once(std::string { COMPLETE_CONFIG }, "num_io_queues = 65534", "num_io_queues = 1");
    document = replace_once(document, "admin_command_timeout_ms = 10000", "admin_command_timeout_ms = 4294967295");

    auto result = ConfigLoader::parse(document, "device-boundaries.toml");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->device_policy.default_options.num_io_queues, 1u);
    EXPECT_EQ(result->device_policy.default_options.admin_command_timeout,
        std::chrono::milliseconds { std::numeric_limits<uint32_t>::max() });
}

TEST(ConfigLoaderTest, RejectsInvalidDevicePolicy)
{
    constexpr std::string_view DEFAULT_TABLE = R"([devices.default]
num_io_queues = 65534
admin_command_timeout_ms = 10000

)";
    struct Case {
        std::string document;
        std::string_view path;
    };
    const Case CASES[] = {
        { replace_once(std::string { COMPLETE_CONFIG }, DEFAULT_TABLE, "[devices]\ndefault = 1\n\n"),
            "devices.default" },
        { replace_once(
              std::string { COMPLETE_CONFIG }, "[devices.default]", "[devices]\n\"03:00.0\" = 1\n\n[devices.default]"),
            "devices.03:00.0" },
        { replace_once(std::string { COMPLETE_CONFIG }, DEFAULT_TABLE,
              "[devices.default]\nnum_io_queues = 65534\nadmin_command_timeout_ms = 10000\nunknown = true\n\n"),
            "devices.default.unknown" },
        { replace_once(std::string { COMPLETE_CONFIG }, "num_io_queues = 65534", "num_io_queues = 0"),
            "devices.default.num_io_queues" },
        { replace_once(std::string { COMPLETE_CONFIG }, "num_io_queues = 65534", "num_io_queues = 65535"),
            "devices.default.num_io_queues" },
        { replace_once(std::string { COMPLETE_CONFIG }, "num_io_queues = 65534", "num_io_queues = 4.0"),
            "devices.default.num_io_queues" },
        { replace_once(std::string { COMPLETE_CONFIG }, "num_io_queues = 65534", "num_io_queues = \"4\""),
            "devices.default.num_io_queues" },
        { replace_once(
              std::string { COMPLETE_CONFIG }, "admin_command_timeout_ms = 10000", "admin_command_timeout_ms = 9999"),
            "devices.default.admin_command_timeout_ms" },
        { replace_once(std::string { COMPLETE_CONFIG }, "admin_command_timeout_ms = 10000",
              "admin_command_timeout_ms = 4294967296"),
            "devices.default.admin_command_timeout_ms" },
        { replace_once(std::string { COMPLETE_CONFIG }, "admin_command_timeout_ms = 10000",
              "admin_command_timeout_ms = 10000.0"),
            "devices.default.admin_command_timeout_ms" },
        { replace_once(std::string { COMPLETE_CONFIG }, "admin_command_timeout_ms = 10000",
              "admin_command_timeout_ms = \"10000\""),
            "devices.default.admin_command_timeout_ms" },
        { std::string { COMPLETE_CONFIG } + "\n[devices.not-a-bdf]\nnum_io_queues = 1\n", "devices.not-a-bdf" },
        { std::string { COMPLETE_CONFIG }
                + "\n[devices.\"03:00.0\"]\nnum_io_queues = 1\n"
                  "\n[devices.\"0000:03:00.0\"]\nnum_io_queues = 2\n",
            "devices.03:00.0" },
        { std::string { COMPLETE_CONFIG } + "\n[devices.\"03:00.0\"]\nunknown = true\n", "devices.03:00.0.unknown" },
    };

    for (const auto& test_case : CASES) {
        SCOPED_TRACE(test_case.path);
        expect_error_at(test_case.document, test_case.path);
    }
}

TEST(ConfigLoaderTest, DuplicateDeviceAliasReportsSourceSelectorPath)
{
    const auto document = std::string { COMPLETE_CONFIG }
        + "\n[devices.\"03:0A.0\"]\nnum_io_queues = 1\n"
          "\n[devices.\"03:0a.0\"]\nnum_io_queues = 2\n";

    auto result = ConfigLoader::parse(document, "duplicate-device-alias.toml");
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().path == "devices.03:0A.0" || result.error().path == "devices.03:0a.0")
        << result.error().path;
    EXPECT_EQ(result.error().message, "device selector duplicates canonical BDF '0000:03:0a.0'");
}

TEST(ConfigLoaderTest, DefaultsLoggingPolicyWhenTableIsAbsent)
{
    const auto document = replace_once(std::string { COMPLETE_CONFIG }, "[logging]\nlevel = \"info\"\n\n", "");
    auto result = ConfigLoader::parse(document, "logging-default.toml");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->logging.level, logging::default_level());
}

TEST(ConfigLoaderTest, AcceptsConfiguredLevelsAtOrAboveCompiledFloor)
{
    struct Case {
        std::string_view text;
        LogLevel level;
    };
    constexpr Case CASES[] = {
        { "trace", LogLevel::TRACE },
        { "debug", LogLevel::DEBUG },
        { "info", LogLevel::INFO },
        { "warn", LogLevel::WARN },
        { "error", LogLevel::ERROR },
        { "critical", LogLevel::CRITICAL },
        { "off", LogLevel::OFF },
    };

    for (const auto& test_case : CASES) {
        SCOPED_TRACE(test_case.text);
        const auto replacement = "level = \"" + std::string { test_case.text } + "\"";
        const auto document = replace_once(std::string { COMPLETE_CONFIG }, "level = \"info\"", replacement);
        auto result = ConfigLoader::parse(document, "logging-level.toml");
        if (logging::is_available(test_case.level)) {
            ASSERT_TRUE(result.has_value()) << result.error().message;
            EXPECT_EQ(result->logging.level, test_case.level);
        } else {
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error().path, "logging.level");
            EXPECT_NE(result.error().message.find("unavailable"), std::string::npos);
        }
    }
}

TEST(ConfigLoaderTest, RejectsMalformedLoggingPolicy)
{
    struct Case {
        std::string document;
        std::string_view path;
    };
    const Case CASES[] = {
        { replace_once(std::string { COMPLETE_CONFIG }, "[logging]\nlevel = \"info\"", "logging = \"info\""),
            "logging" },
        { replace_once(std::string { COMPLETE_CONFIG }, "level = \"info\"", "level = 1"), "logging.level" },
        { replace_once(std::string { COMPLETE_CONFIG }, "level = \"info\"", "level = \"verbose\""), "logging.level" },
        { replace_once(std::string { COMPLETE_CONFIG }, "level = \"info\"", "level = \"info\"\nunknown = true"),
            "logging.unknown" },
    };

    for (const auto& test_case : CASES) {
        SCOPED_TRACE(test_case.path);
        expect_error_at(test_case.document, test_case.path);
    }
}

TEST(ConfigLoaderTest, InheritsNamedPrincipalFieldsFromDefault)
{
    auto result = ConfigLoader::parse(COMPLETE_CONFIG, "inheritance.toml");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto& limits = result->quota_policy.principals.at("client_app");
    EXPECT_EQ(limits.sessions, 8);
    EXPECT_EQ(limits.transferred_fds, 8);
    EXPECT_EQ(limits.mapped_buffers, 4);
    EXPECT_EQ(limits.device_handles, 2);
}

TEST(ConfigLoaderTest, ParsesPerFieldUnlimitedLimits)
{
    auto document = replace_once(std::string { COMPLETE_CONFIG }, "sessions = 64", "sessions = \"unlimited\"");
    document = replace_once(std::move(document), "max_buffer_bytes = \"8GiB\"", "max_buffer_bytes = \"unlimited\"");
    document = replace_once(std::move(document), "transferred_fds = 8", "transferred_fds = \"unlimited\"");

    auto result = ConfigLoader::parse(document, "mixed.toml");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->quota_policy.global.sessions, std::nullopt);
    EXPECT_EQ(result->quota_policy.global.max_buffer_bytes, std::nullopt);
    EXPECT_EQ(result->quota_policy.default_principal.transferred_fds, std::nullopt);
    EXPECT_EQ(result->quota_policy.global.mapped_buffers, 128);
}

TEST(ConfigLoaderTest, ParsesWholePolicyUnlimitedWithMappings)
{
    constexpr std::string_view DOCUMENT = R"(
schema_version = 1
[sockets.api]
path = "/run/storage-lender/api.sock"
owner = "root"
group = "storage-lender"
mode = 0o660
[devices.default]
num_io_queues = 65534
admin_command_timeout_ms = 10000
[quotas]
mode = "unlimited"
[principals.camera]
uids = [1001, 1002]
[principals.storage]
gids = [2001]
)";

    auto result = ConfigLoader::parse(DOCUMENT, "unlimited.toml");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->quota_policy.mode, QuotaMode::UNLIMITED);
    ASSERT_TRUE(result->quota_policy.principals.contains("camera"));
    EXPECT_EQ(result->quota_policy.principals.at("camera"), QuotaLimits { });
    EXPECT_EQ(result->principal_resolver.resolve(1002, 2001), "camera");
    EXPECT_EQ(result->principal_resolver.resolve(9999, 2001), "storage");
}

TEST(ConfigLoaderTest, RejectsLimitTablesInUnlimitedMode)
{
    struct Case {
        std::string_view document;
        std::string_view path;
    };
    constexpr Case CASES[] = {
        { R"(schema_version = 1
[sockets.api]
path = "/run/storage-lender/api.sock"
owner = "root"
group = "storage-lender"
mode = 0o660
[devices.default]
num_io_queues = 65534
admin_command_timeout_ms = 10000
[quotas]
mode = "unlimited"
[quotas.global]
sessions = 1
)",
            "quotas.global" },
        { R"(schema_version = 1
[sockets.api]
path = "/run/storage-lender/api.sock"
owner = "root"
group = "storage-lender"
mode = 0o660
[devices.default]
num_io_queues = 65534
admin_command_timeout_ms = 10000
[quotas]
mode = "unlimited"
[quotas.default]
sessions = 1
)",
            "quotas.default" },
        { R"(schema_version = 1
[sockets.api]
path = "/run/storage-lender/api.sock"
owner = "root"
group = "storage-lender"
mode = 0o660
[devices.default]
num_io_queues = 65534
admin_command_timeout_ms = 10000
[quotas]
mode = "unlimited"
[principals.camera]
uids = [1001]
[principals.camera.quotas]
sessions = 1
)",
            "principals.camera.quotas" },
    };

    for (const auto& test_case : CASES) {
        SCOPED_TRACE(test_case.path);
        expect_error_at(test_case.document, test_case.path);
    }
}

TEST(ConfigLoaderTest, RejectsNonStringSocketTextFields)
{
    struct Case {
        std::string_view original;
        std::string_view replacement;
        std::string_view path;
    };
    constexpr Case CASES[] = {
        { "path = \"/run/storage-lender/api.sock\"", "path = 7", "sockets.api.path" },
        { "owner = \"root\"", "owner = [\"root\"]", "sockets.api.owner" },
        { "group = \"storage-lender\"", "group = true", "sockets.api.group" },
    };

    for (const auto& test_case : CASES) {
        SCOPED_TRACE(test_case.path);
        const auto document = replace_once(std::string { COMPLETE_CONFIG }, test_case.original, test_case.replacement);
        expect_error_at(document, test_case.path);
    }
}

TEST(ConfigLoaderTest, RejectsInvalidSocketPaths)
{
    constexpr std::string_view INVALID_PATHS[]
        = { "", "relative/api.sock", "/run/storage-lender/", "/run/./api.sock", "/run/../api.sock" };

    for (const auto path : INVALID_PATHS) {
        SCOPED_TRACE(path);
        const auto replacement = "path = \"" + std::string { path } + "\"";
        const auto document
            = replace_once(std::string { COMPLETE_CONFIG }, "path = \"/run/storage-lender/api.sock\"", replacement);
        expect_error_at(document, "sockets.api.path");
    }

    const auto overlong_path = "/" + std::string(sizeof(sockaddr_un::sun_path), 'a');
    const auto replacement = "path = \"" + overlong_path + "\"";
    const auto document
        = replace_once(std::string { COMPLETE_CONFIG }, "path = \"/run/storage-lender/api.sock\"", replacement);
    expect_error_at(document, "sockets.api.path");
}

TEST(ConfigLoaderTest, RejectsEmptySocketOwnerOrGroup)
{
    struct Case {
        std::string_view original;
        std::string_view replacement;
        std::string_view path;
    };
    constexpr Case CASES[] = {
        { "owner = \"root\"", "owner = \"\"", "sockets.api.owner" },
        { "group = \"storage-lender\"", "group = \"\"", "sockets.api.group" },
    };

    for (const auto& test_case : CASES) {
        SCOPED_TRACE(test_case.path);
        const auto document = replace_once(std::string { COMPLETE_CONFIG }, test_case.original, test_case.replacement);
        expect_error_at(document, test_case.path);
    }
}

TEST(ConfigLoaderTest, RejectsInvalidSocketModes)
{
    constexpr std::string_view INVALID_MODES[]
        = { "0o000", "0o400", "0o601", "0o610", "0o700", "0o1660", "-1", "438.0", "\"0660\"" };

    for (const auto mode : INVALID_MODES) {
        SCOPED_TRACE(mode);
        const auto replacement = "mode = " + std::string { mode };
        const auto document = replace_once(std::string { COMPLETE_CONFIG }, "mode = 0o660", replacement);
        expect_error_at(document, "sockets.api.mode");
    }
}

TEST(ConfigLoaderTest, ParsesAllowedSocketModes)
{
    constexpr std::string_view VALID_MODES[] = { "0o600", "0o620", "0o640", "0o660" };

    for (const auto mode : VALID_MODES) {
        SCOPED_TRACE(mode);
        const auto replacement = "mode = " + std::string { mode };
        const auto document = replace_once(std::string { COMPLETE_CONFIG }, "mode = 0o660", replacement);
        const auto result = ConfigLoader::parse(document, "socket-mode.toml");
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }
}

TEST(ConfigLoaderTest, RejectsUnknownSchemaVersion)
{
    for (auto replacement :
        { std::string_view { "schema_version = 2" }, std::string_view { "schema_version = 1.0" } }) {
        SCOPED_TRACE(replacement);
        auto error = parse_error(replace_once(std::string { COMPLETE_CONFIG }, "schema_version = 1", replacement));
        EXPECT_EQ(error.path, "schema_version");
        EXPECT_NE(error.message.find("1"), std::string::npos);
    }
}

TEST(ConfigLoaderTest, RejectsUnknownFieldsAtEveryTableLevel)
{
    struct Case {
        std::string document;
        std::string_view path;
    };
    const Case CASES[] = {
        { "unknown = true\n" + std::string { COMPLETE_CONFIG }, "unknown" },
        { replace_once(std::string { COMPLETE_CONFIG }, "level = \"info\"", "level = \"info\"\nunknown = true"),
            "logging.unknown" },
        { replace_once(std::string { COMPLETE_CONFIG }, "[sockets.api]", "[sockets]\nunknown = true\n[sockets.api]"),
            "sockets.unknown" },
        { replace_once(std::string { COMPLETE_CONFIG }, "mode = 0o660", "mode = 0o660\nunknown = true"),
            "sockets.api.unknown" },
        { replace_once(std::string { COMPLETE_CONFIG }, "mode = \"enforced\"", "mode = \"enforced\"\nunknown = true"),
            "quotas.unknown" },
        { replace_once(std::string { COMPLETE_CONFIG }, "sessions = 64", "sessions = 64\nunknown = true"),
            "quotas.global.unknown" },
        { replace_once(std::string { COMPLETE_CONFIG }, "sessions = 2", "sessions = 2\nunknown = true"),
            "quotas.default.unknown" },
        { replace_once(std::string { COMPLETE_CONFIG }, "[principals.client_app]",
              "[principals]\nunknown = true\n[principals.client_app]"),
            "principals.unknown" },
        { replace_once(std::string { COMPLETE_CONFIG }, "gids = [2001]", "gids = [2001]\nunknown = true"),
            "principals.client_app.unknown" },
        { replace_once(std::string { COMPLETE_CONFIG }, "sessions = 8", "sessions = 8\nunknown = true"),
            "principals.client_app.quotas.unknown" },
    };

    for (const auto& test_case : CASES) {
        SCOPED_TRACE(test_case.path);
        expect_error_at(test_case.document, test_case.path);
    }
}

TEST(ConfigLoaderTest, RejectsDuplicateUidMappings)
{
    auto document = std::string { COMPLETE_CONFIG } + R"(
[principals.camera]
uids = [1001]
)";
    auto error = parse_error(document);
    EXPECT_EQ(error.path, "principals.camera.uids");
    EXPECT_NE(error.message.find("1001"), std::string::npos);
    EXPECT_NE(error.message.find("client_app"), std::string::npos);
}

TEST(ConfigLoaderTest, RejectsDuplicateGidMappings)
{
    auto document = std::string { COMPLETE_CONFIG } + R"(
[principals.camera]
gids = [2001]
)";
    auto error = parse_error(document);
    EXPECT_EQ(error.path, "principals.camera.gids");
    EXPECT_NE(error.message.find("2001"), std::string::npos);
    EXPECT_NE(error.message.find("client_app"), std::string::npos);
}

TEST(ConfigLoaderTest, RejectsPrincipalWithoutUidOrGid)
{
    auto document = replace_once(std::string { COMPLETE_CONFIG }, "uids = [1001]\ngids = [2001]\n", "");
    expect_error_at(document, "principals.client_app");
}

TEST(ConfigLoaderTest, RejectsReservedDefaultPrincipalName)
{
    constexpr std::string_view DOCUMENT = R"(
schema_version = 1
[sockets.api]
path = "/run/storage-lender/api.sock"
owner = "root"
group = "storage-lender"
mode = 0o660
[devices.default]
num_io_queues = 65534
admin_command_timeout_ms = 10000
[quotas]
mode = "unlimited"
[principals.default]
uids = [1001]
)";

    auto error = parse_error(DOCUMENT);
    EXPECT_EQ(error.path, "principals.default");
    EXPECT_NE(error.message.find("reserved"), std::string::npos);
}

TEST(ConfigLoaderTest, RejectsInfinityNegativeFloatBadUnitAndOverflow)
{
    struct Case {
        std::string_view original;
        std::string_view replacement;
        std::string_view path;
    };
    constexpr Case CASES[] = {
        { "sessions = 64", "sessions = \"infinity\"", "quotas.global.sessions" },
        { "sessions = 64", "sessions = -1", "quotas.global.sessions" },
        { "sessions = 64", "sessions = 1.5", "quotas.global.sessions" },
        { "sessions = 64", "sessions = 1.0", "quotas.global.sessions" },
        { "max_buffer_bytes = \"8GiB\"", "max_buffer_bytes = \"8GB\"", "quotas.global.max_buffer_bytes" },
        { "max_buffer_bytes = \"8GiB\"", "max_buffer_bytes = \"18446744073709551615GiB\"",
            "quotas.global.max_buffer_bytes" },
    };

    for (const auto& test_case : CASES) {
        SCOPED_TRACE(test_case.replacement);
        auto document = replace_once(std::string { COMPLETE_CONFIG }, test_case.original, test_case.replacement);
        expect_error_at(document, test_case.path);
    }
}

TEST(ConfigLoaderTest, ParsesBytesAndBinarySizeSuffixes)
{
    struct Case {
        std::string_view value;
        uint64_t expected;
    };
    constexpr Case CASES[] = {
        { "7", 7 },
        { "\"7B\"", 7 },
        { "\"7KiB\"", 7 * 1024 },
        { "\"7MiB\"", 7 * 1024 * 1024 },
        { "\"7GiB\"", 7ULL * 1024 * 1024 * 1024 },
    };

    for (const auto& test_case : CASES) {
        SCOPED_TRACE(test_case.value);
        auto replacement = "max_buffer_bytes = " + std::string { test_case.value };
        auto document = replace_once(std::string { COMPLETE_CONFIG }, "max_buffer_bytes = \"8GiB\"", replacement);
        auto result = ConfigLoader::parse(document, "sizes.toml");
        ASSERT_TRUE(result.has_value()) << result.error().message;
        EXPECT_EQ(result->quota_policy.global.max_buffer_bytes, test_case.expected);
    }
}

TEST(ConfigLoaderTest, ZeroIsFiniteRatherThanUnlimited)
{
    auto document = replace_once(std::string { COMPLETE_CONFIG }, "sessions = 64", "sessions = 0");
    document = replace_once(std::move(document), "max_buffer_bytes = \"8GiB\"", "max_buffer_bytes = \"0B\"");
    auto result = ConfigLoader::parse(document, "zero.toml");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(result->quota_policy.global.sessions.has_value());
    ASSERT_TRUE(result->quota_policy.global.max_buffer_bytes.has_value());
    EXPECT_EQ(*result->quota_policy.global.sessions, 0);
    EXPECT_EQ(*result->quota_policy.global.max_buffer_bytes, 0);
}

TEST(ConfigLoaderTest, PackagedConfigurationExplicitlySelectsUnlimitedPolicy)
{
    auto result = ConfigLoader::load(STORAGE_LENDER_SOURCE_DIR "/config/storage-lender.toml");
    ASSERT_TRUE(result.has_value()) << result.error().source << ": " << result.error().message;
    EXPECT_EQ(result->logging.level, LogLevel::INFO);
    EXPECT_EQ(result->api_socket.path, "/run/storage-lender/api.sock");
    EXPECT_EQ(result->ctl_socket.path, "/run/storage-lender/ctl.sock");
    EXPECT_EQ(result->device_policy.default_options.num_io_queues, 65534u);
    EXPECT_EQ(result->device_policy.default_options.admin_command_timeout, std::chrono::milliseconds { 10000 });
    EXPECT_EQ(result->quota_policy.mode, QuotaMode::UNLIMITED);
}

TEST(ConfigLoaderTest, StartupMissingFileUsesBuiltinDefaults)
{
    const std::string path = STORAGE_LENDER_SOURCE_DIR "/config/does-not-exist.toml";
    auto result = ConfigLoader::load_for_startup(path);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->used_builtin_defaults);
    expect_builtin_defaults(result->config);
}

TEST(ConfigLoaderTest, StrictLoadStillRejectsMissingFile)
{
    const std::string path = STORAGE_LENDER_SOURCE_DIR "/config/does-not-exist.toml";
    auto result = ConfigLoader::load(path);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ConfigErrorKind::FILE_NOT_FOUND);
    EXPECT_EQ(result.error().source, path);
    EXPECT_EQ(result.error().path, "$file");
    EXPECT_FALSE(result.error().message.empty());
}

TEST(ConfigLoaderTest, StartupDoesNotDefaultOtherFileErrors)
{
    auto result = ConfigLoader::load_for_startup(STORAGE_LENDER_SOURCE_DIR "/config");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ConfigErrorKind::FILE_IO);
}
