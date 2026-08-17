# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
# in compliance with the License. You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the License
# is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
# or implied. See the License for the specific language governing permissions and limitations under
# the License.

add_library(
    StorageLenderServerTestSupport STATIC
    support/ctl_test_client.cpp support/protocol_test_client.cpp support/server_test_harness.cpp
    support/test_dma_buffer.cpp)
add_library(StorageLender::Server::TestSupport ALIAS StorageLenderServerTestSupport)
add_dependencies(StorageLenderServerTestSupport CheckNoExceptions)
target_include_directories(StorageLenderServerTestSupport
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(StorageLenderServerTestSupport PUBLIC StorageLender::Server::Core
                                                            GTest::gmock)

add_lender_test(
    NAME
    StorageLenderServerBufferManagerTest
    SOURCES
    unit/buffer_manager_test.cpp
    LIBRARIES
    StorageLender::Server::TestSupport
    LABELS
    server
    unit)

add_lender_test(
    NAME
    StorageLenderServerPciAddressTest
    SOURCES
    unit/pci_address_test.cpp
    LIBRARIES
    StorageLender::Server::Core
    LABELS
    server
    unit)

add_lender_test(
    NAME
    StorageLenderServerCommandMetricsTest
    SOURCES
    unit/command_metrics_test.cpp
    LIBRARIES
    StorageLender::Server::Core
    LABELS
    server
    unit)

add_lender_test(
    NAME
    StorageLenderServerDeviceManagerTest
    SOURCES
    unit/device_manager_test.cpp
    LIBRARIES
    StorageLender::Server::TestSupport
    LABELS
    server
    unit)

add_lender_test(
    NAME
    StorageLenderServerQueueManagerTest
    SOURCES
    unit/queue_manager_test.cpp
    LIBRARIES
    StorageLender::Server::TestSupport
    LABELS
    server
    unit)

add_lender_test(
    NAME
    StorageLenderServerPrincipalResolverTest
    SOURCES
    unit/principal_resolver_test.cpp
    LIBRARIES
    StorageLender::Server::Core
    LABELS
    server
    unit)

add_lender_test(
    NAME
    StorageLenderServerQuotaManagerTest
    SOURCES
    unit/quota_manager_test.cpp
    LIBRARIES
    StorageLender::Server::Core
    LABELS
    server
    unit)

add_lender_test(
    NAME
    StorageLenderServerConfigLoaderTest
    SOURCES
    unit/config_loader_test.cpp
    LIBRARIES
    StorageLender::Server::Core
    LABELS
    server
    unit)
target_compile_definitions(StorageLenderServerConfigLoaderTest
                           PRIVATE STORAGE_LENDER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_lender_test(
    NAME
    StorageLenderServerLoggingTest
    SOURCES
    unit/logging_test.cpp
    LIBRARIES
    StorageLender::Server::Core
    LABELS
    server
    unit)

add_lender_test(
    NAME
    StorageLenderServerPosixRaiiTest
    SOURCES
    unit/posix_raii_test.cpp
    LIBRARIES
    StorageLender::Server::Core
    LABELS
    server
    unit)

add_lender_test(
    NAME
    StorageLenderServerOptionsTest
    SOURCES
    unit/server_options_test.cpp
    LIBRARIES
    StorageLender::Server::Core
    LABELS
    server
    unit)

add_lender_test(
    NAME
    StorageLenderServerSpdkAdminCommandTest
    SOURCES
    unit/spdk_admin_command_test.cpp
    LIBRARIES
    StorageLender::Server::Core
    LABELS
    server
    unit)

add_lender_test(
    NAME
    StorageLenderServerUnixSocketEndpointTest
    SOURCES
    unit/unix_socket_endpoint_test.cpp
    LIBRARIES
    StorageLender::Server::Core
    LABELS
    server
    unit)

add_lender_test(
    NAME
    StorageLenderServerAcceptErrorTest
    SOURCES
    unit/accept_error_test.cpp
    LIBRARIES
    StorageLender::Server::Core
    LABELS
    server
    unit)

add_lender_test(
    NAME
    StorageLenderServerProtocolIntegrationTest
    SOURCES
    integration/protocol_test.cpp
    LIBRARIES
    StorageLender::Server::TestSupport
    LABELS
    server
    integration)

add_lender_test(
    NAME
    StorageLenderServerManagementIntegrationTest
    SOURCES
    integration/management_test.cpp
    LIBRARIES
    StorageLender::Server::TestSupport
    LABELS
    server
    integration)

add_lender_test(
    NAME
    StorageLenderServerSessionIntegrationTest
    SOURCES
    integration/session_test.cpp
    LIBRARIES
    StorageLender::Server::TestSupport
    LABELS
    server
    integration)

add_lender_test(
    NAME
    StorageLenderServerBufferIntegrationTest
    SOURCES
    integration/buffer_test.cpp
    LIBRARIES
    StorageLender::Server::TestSupport
    LABELS
    server
    integration)

add_lender_test(
    NAME
    StorageLenderServerDeviceIntegrationTest
    SOURCES
    integration/device_test.cpp
    LIBRARIES
    StorageLender::Server::TestSupport
    LABELS
    server
    integration)

add_lender_test(
    NAME
    StorageLenderServerQueueIntegrationTest
    SOURCES
    integration/queue_test.cpp
    LIBRARIES
    StorageLender::Server::TestSupport
    LABELS
    server
    integration)

add_lender_test(
    NAME
    StorageLenderServerQuotaIntegrationTest
    SOURCES
    integration/quota_test.cpp
    LIBRARIES
    StorageLender::Server::TestSupport
    LABELS
    server
    integration)

add_lender_test(
    NAME
    StorageLenderServerReloadIntegrationTest
    SOURCES
    integration/reload_test.cpp
    LIBRARIES
    StorageLender::Server::TestSupport
    LABELS
    server
    integration)

add_lender_test(
    NAME
    StorageLenderServerResiliencyIntegrationTest
    SOURCES
    integration/resiliency_test.cpp
    LIBRARIES
    StorageLender::Server::TestSupport
    LABELS
    server
    integration)

add_lender_test(
    NAME
    StorageLenderServerSpdkBackendTest
    SOURCES
    hardware/spdk_backend_test.cpp
    LIBRARIES
    StorageLender::Server::Core
    LABELS
    server
    hardware)
