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

include(GoogleTest)

function(add_lender_test)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "NAME" "SOURCES;LIBRARIES;LABELS")

    if(NOT ARG_NAME)
        message(FATAL_ERROR "add_lender_test requires NAME")
    endif()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "add_lender_test requires SOURCES")
    endif()

    add_executable(${ARG_NAME} ${ARG_SOURCES})
    add_dependencies(${ARG_NAME} CheckNoExceptions)
    target_link_libraries(${ARG_NAME} PRIVATE GTest::gtest_main GTest::gmock ${ARG_LIBRARIES})
    gtest_discover_tests(${ARG_NAME})

    # Discovered tests do not exist at configure time, so CTest applies their labels later.
    set(LABEL_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/${ARG_NAME}_labels.cmake")
    string(
        CONCAT LABEL_SCRIPT_CONTENT "foreach(TEST_NAME IN LISTS ${ARG_NAME}_TESTS)\n"
               "    set_tests_properties(\"\${TEST_NAME}\" PROPERTIES LABELS \"${ARG_LABELS}\")\n"
               "endforeach()\n")
    file(
        GENERATE
        OUTPUT "${LABEL_SCRIPT}"
        CONTENT "${LABEL_SCRIPT_CONTENT}")
    set_property(
        DIRECTORY
        APPEND
        PROPERTY TEST_INCLUDE_FILES "${LABEL_SCRIPT}")
endfunction()
