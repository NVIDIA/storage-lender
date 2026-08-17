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
set(SOURCE_FILES)
foreach(SOURCE_DIRECTORY client examples server tests)
    file(
        GLOB_RECURSE DIRECTORY_SOURCE_FILES
        LIST_DIRECTORIES false
        "${SOURCE_DIR}/${SOURCE_DIRECTORY}/*.cc"
        "${SOURCE_DIR}/${SOURCE_DIRECTORY}/*.cpp"
        "${SOURCE_DIR}/${SOURCE_DIRECTORY}/*.cxx"
        "${SOURCE_DIR}/${SOURCE_DIRECTORY}/*.h"
        "${SOURCE_DIR}/${SOURCE_DIRECTORY}/*.hh"
        "${SOURCE_DIR}/${SOURCE_DIRECTORY}/*.hpp"
        "${SOURCE_DIR}/${SOURCE_DIRECTORY}/*.hxx")
    list(APPEND SOURCE_FILES ${DIRECTORY_SOURCE_FILES})
endforeach()

set(EXCEPTION_KEYWORD_PATTERN "(^|[^A-Za-z0-9_])(throw|try|catch)([^A-Za-z0-9_]|$)")
foreach(SOURCE_FILE IN LISTS SOURCE_FILES)
    file(STRINGS "${SOURCE_FILE}" EXCEPTION_LINES REGEX "${EXCEPTION_KEYWORD_PATTERN}")
    if(EXCEPTION_LINES)
        list(JOIN EXCEPTION_LINES "\n  " FORMATTED_LINES)
        message(FATAL_ERROR "C++ exception keyword found in ${SOURCE_FILE}:\n  ${FORMATTED_LINES}")
    endif()
endforeach()
