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
find_package(PkgConfig REQUIRED)
pkg_check_modules(Systemd REQUIRED IMPORTED_TARGET libsystemd)

add_library(
    StorageLenderServerCore STATIC
    src/server.cpp
    src/accept_error.cpp
    src/server_options.cpp
    src/command_metrics.cpp
    src/buffer_manager.cpp
    src/config_loader.cpp
    src/device_manager.cpp
    src/logging.cpp
    src/management_server.cpp
    src/pci_address.cpp
    src/principal_resolver.cpp
    src/quota_manager.cpp
    src/queue_manager.cpp
    src/socket_config.cpp
    src/spdk_admin_command.cpp
    src/spdk_backend.cpp
    src/unix_socket_endpoint.cpp)
add_library(StorageLender::Server::Core ALIAS StorageLenderServerCore)

add_dependencies(StorageLenderServerCore CheckNoExceptions)
enable_boost_coroutine_exceptions(StorageLenderServerCore)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(StorageLenderServerCore INTERFACE -fexceptions)
endif()

target_include_directories(StorageLenderServerCore PUBLIC ${CMAKE_SOURCE_DIR}/server/include)

target_compile_definitions(
    StorageLenderServerCore PUBLIC $<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
                                   $<$<NOT:$<CONFIG:Debug>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG>)

target_link_libraries(
    StorageLenderServerCore
    PUBLIC StorageLenderWireProto
           StorageLenderClientProto
           StorageLenderCtlProto
           StorageLender::Wire::Framing
           Boost::context
           Boost::coroutine
           Boost::fiber
           Boost::headers
           Boost::system
           PkgConfig::Systemd
           protobuf::libprotobuf
           SPDK::nvme
           spdlog::spdlog
           tomlplusplus::tomlplusplus)

add_executable(StorageLenderServer src/main.cpp)
add_dependencies(StorageLenderServer CheckNoExceptions)
target_link_libraries(StorageLenderServer PRIVATE StorageLender::Server::Core)
set_target_properties(StorageLenderServer PROPERTIES OUTPUT_NAME storage-lender-server)

if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS StorageLenderServer RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(
    FILES ${CMAKE_SOURCE_DIR}/config/storage-lender.toml
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/storage-lender
    RENAME config.toml)
