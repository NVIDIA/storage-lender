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
cmake_minimum_required(VERSION 3.26)

project(
    "storage-lender"
    VERSION 0.1.0
    DESCRIPTION "Storage Lender Service"
    LANGUAGES CXX)

add_custom_target(
    CheckNoExceptions ALL
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P
            ${CMAKE_SOURCE_DIR}/cmake/CheckNoExceptions.cmake
    COMMENT "Checking that project sources do not use C++ exceptions")

function(enable_boost_coroutine_exceptions TARGET)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${TARGET} PRIVATE -fexceptions)
    endif()
endfunction()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(GNUInstallDirs)
include(CPM)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_definitions(SPDLOG_NO_EXCEPTIONS)
    add_compile_options(-Wall -Wextra -Werror -fno-exceptions $<$<CONFIG:Debug>:-O0>
                        $<$<NOT:$<CONFIG:Debug>>:-O3>)
endif()

if(PROJECT_IS_TOP_LEVEL)
    option(BUILD_EXAMPLES "Build examples" ON)
    option(BUILD_TESTS "Build tests" ON)
else()
    option(BUILD_EXAMPLES "Build examples" OFF)
    option(BUILD_TESTS "Build tests" OFF)
endif()
option(ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(ENABLE_COVERAGE "Enable coverage instrumentation and report target (requires gcovr)" OFF)

if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

if(ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "ENABLE_COVERAGE requires GCC or Clang")
    endif()
    set(BUILD_TESTS
        ON
        CACHE BOOL "Force tests on for coverage" FORCE)
    add_compile_options(--coverage)
    add_link_options(--coverage)
endif()

# Use BoostConfig.cmake instead of the FindBoost module.
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

# Dependencies.
find_package(Boost REQUIRED CONFIG COMPONENTS fiber context system coroutine)
find_package(Protobuf REQUIRED)
find_package(SPDK REQUIRED)

cpmaddpackage("gh:gabime/spdlog@1.17.0")
cpmaddpackage("gh:marzer/tomlplusplus@3.4.0")

if(BUILD_TESTS)
    cpmaddpackage("gh:google/googletest@1.17.0")
    enable_testing()
    include(AddLenderTest)
endif()

add_subdirectory(proto)
add_subdirectory(wire)
add_subdirectory(server)
add_subdirectory(client)
add_subdirectory(ctl)

if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

add_custom_target(
    NoticeBundle
    COMMAND
        ${CMAKE_SOURCE_DIR}/ci/create-notice-bundle.sh ${CMAKE_BINARY_DIR} ${SPDK_NOTICE_SOURCE_DIR}
        ${spdlog_SOURCE_DIR} ${tomlplusplus_SOURCE_DIR} $<TARGET_FILE:StorageLenderServer>
        $<TARGET_FILE:StorageLenderCtl> $<TARGET_FILE:StorageLenderClientShared>
    DEPENDS StorageLenderServer StorageLenderCtl StorageLenderClientShared
    COMMENT "Creating the third-party open-source notice bundle"
    VERBATIM USES_TERMINAL)

if(ENABLE_COVERAGE)
    find_program(GCOVR gcovr REQUIRED)
    set(COVERAGE_DIR "${CMAKE_BINARY_DIR}/coverage")
    add_custom_target(
        Coverage
        COMMAND ${CMAKE_COMMAND} -E make_directory "${COVERAGE_DIR}"
        COMMAND find "${CMAKE_BINARY_DIR}" -name "*.gcda" -delete
        COMMAND ${CMAKE_CTEST_COMMAND} -Q --test-dir "${CMAKE_BINARY_DIR}"
        COMMAND
            ${GCOVR} --root "${CMAKE_SOURCE_DIR}" --gcov-ignore-errors=no_working_dir_found --filter
            "${CMAKE_SOURCE_DIR}/client/include/" --filter "${CMAKE_SOURCE_DIR}/client/src/"
            --filter "${CMAKE_SOURCE_DIR}/ctl/src/" --filter "${CMAKE_SOURCE_DIR}/server/include/"
            --filter "${CMAKE_SOURCE_DIR}/server/src/" --filter "${CMAKE_SOURCE_DIR}/wire/include/"
            --filter "${CMAKE_SOURCE_DIR}/wire/src/" --html-details "${COVERAGE_DIR}/index.html"
            --print-summary "${CMAKE_BINARY_DIR}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Generating coverage report in ${COVERAGE_DIR}/")
endif()
