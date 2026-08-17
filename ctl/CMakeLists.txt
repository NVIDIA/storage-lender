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
    StorageLenderCtlCore STATIC
    src/ctl_client.cpp src/options.cpp src/render.cpp $<TARGET_OBJECTS:StorageLenderCtlProto>
    $<TARGET_OBJECTS:StorageLenderWireProto>)
add_library(StorageLender::Ctl::Core ALIAS StorageLenderCtlCore)
add_dependencies(StorageLenderCtlCore CheckNoExceptions)
target_include_directories(
    StorageLenderCtlCore
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
           $<TARGET_PROPERTY:StorageLenderCtlProto,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(
    StorageLenderCtlCore
    PUBLIC protobuf::libprotobuf StorageLender::Wire::Framing
    PRIVATE StorageLenderCtlProto StorageLenderWireProto)
set_target_properties(
    StorageLenderCtlCore
    PROPERTIES CXX_STANDARD 23
               CXX_STANDARD_REQUIRED ON
               CXX_EXTENSIONS OFF)

add_executable(StorageLenderCtl src/main.cpp)
add_dependencies(StorageLenderCtl CheckNoExceptions)
target_compile_definitions(StorageLenderCtl PRIVATE STORAGE_LENDER_VERSION="${PROJECT_VERSION}")
target_link_libraries(StorageLenderCtl PRIVATE StorageLender::Ctl::Core)
set_target_properties(
    StorageLenderCtl
    PROPERTIES CXX_STANDARD 23
               CXX_STANDARD_REQUIRED ON
               CXX_EXTENSIONS OFF
               OUTPUT_NAME storage-lender-ctl)

include(GNUInstallDirs)
install(TARGETS StorageLenderCtl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(BUILD_TESTS)
    add_lender_test(
        NAME
        StorageLenderCtlOptionsTest
        SOURCES
        tests/options_test.cpp
        LIBRARIES
        StorageLender::Ctl::Core
        LABELS
        ctl
        unit)

    add_lender_test(
        NAME
        StorageLenderCtlClientTest
        SOURCES
        tests/ctl_client_test.cpp
        LIBRARIES
        StorageLender::Ctl::Core
        LABELS
        ctl
        unit)

    add_lender_test(
        NAME
        StorageLenderCtlRenderTest
        SOURCES
        tests/render_test.cpp
        LIBRARIES
        StorageLender::Ctl::Core
        LABELS
        ctl
        unit)

    add_lender_test(
        NAME
        StorageLenderCtlIntegrationTest
        SOURCES
        tests/integration_test.cpp
        LIBRARIES
        StorageLender::Ctl::Core
        StorageLender::Server::TestSupport
        LABELS
        ctl
        integration)
    add_dependencies(StorageLenderCtlIntegrationTest StorageLenderCtl)
    target_compile_definitions(StorageLenderCtlIntegrationTest
                               PRIVATE STORAGE_LENDER_CTL_PATH="$<TARGET_FILE:StorageLenderCtl>")
endif()
