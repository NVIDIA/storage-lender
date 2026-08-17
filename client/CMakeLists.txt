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
add_library(StorageLenderClientObjects OBJECT src/client.cpp)
add_dependencies(StorageLenderClientObjects CheckNoExceptions StorageLenderClientProto)
target_include_directories(
    StorageLenderClientObjects
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_SOURCE_DIR}/server/include
            $<TARGET_PROPERTY:StorageLenderClientProto,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(
    StorageLenderClientObjects
    PUBLIC protobuf::libprotobuf
    PRIVATE StorageLenderClientProto StorageLenderWireProto StorageLender::Wire::Framing)
set_target_properties(
    StorageLenderClientObjects
    PROPERTIES POSITION_INDEPENDENT_CODE ON
               CXX_STANDARD 17
               CXX_STANDARD_REQUIRED ON
               CXX_EXTENSIONS OFF)

add_library(
    StorageLenderClientStatic STATIC
    $<TARGET_OBJECTS:StorageLenderClientObjects> $<TARGET_OBJECTS:StorageLenderClientProto>
    $<TARGET_OBJECTS:StorageLenderWireProto> $<TARGET_OBJECTS:StorageLenderWireFramingObjects>)
target_include_directories(
    StorageLenderClientStatic PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                     $<INSTALL_INTERFACE:include>)
target_link_libraries(StorageLenderClientStatic PUBLIC protobuf::libprotobuf)
set_target_properties(StorageLenderClientStatic PROPERTIES OUTPUT_NAME storage_lender_client
                                                           EXPORT_NAME Client::Static)
add_library(StorageLender::Client::Static ALIAS StorageLenderClientStatic)

add_library(
    StorageLenderClientShared SHARED
    $<TARGET_OBJECTS:StorageLenderClientObjects> $<TARGET_OBJECTS:StorageLenderClientProto>
    $<TARGET_OBJECTS:StorageLenderWireProto> $<TARGET_OBJECTS:StorageLenderWireFramingObjects>)
target_include_directories(
    StorageLenderClientShared PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                     $<INSTALL_INTERFACE:include>)
target_link_libraries(StorageLenderClientShared PUBLIC protobuf::libprotobuf)
set_target_properties(
    StorageLenderClientShared
    PROPERTIES OUTPUT_NAME storage_lender_client
               VERSION ${PROJECT_VERSION}
               SOVERSION ${PROJECT_VERSION_MAJOR}
               EXPORT_NAME Client::Shared)
add_library(StorageLender::Client::Shared ALIAS StorageLenderClientShared)

if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(STORAGE_LENDER_CLIENT_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/StorageLenderClient")

install(
    TARGETS StorageLenderClientStatic StorageLenderClientShared
    EXPORT StorageLenderClientTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/storage_lender DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(
    EXPORT StorageLenderClientTargets
    NAMESPACE StorageLender::
    DESTINATION ${STORAGE_LENDER_CLIENT_CMAKE_DIR})

configure_package_config_file(
    ${CMAKE_SOURCE_DIR}/cmake/StorageLenderClientConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/StorageLenderClientConfig.cmake
    INSTALL_DESTINATION ${STORAGE_LENDER_CLIENT_CMAKE_DIR})

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/StorageLenderClientConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/StorageLenderClientConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/StorageLenderClientConfigVersion.cmake
        DESTINATION ${STORAGE_LENDER_CLIENT_CMAKE_DIR})
