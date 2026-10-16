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

# Registers a pytest run as one CTest entry.
#
# add_lender_python_test(NAME <name> PATH <file-or-directory> [PYTHONPATH <dir>...] [LABELS <label>...])
#
# The run uses the interpreter selected by find_package(Python3) and never writes bytecode or a
# pytest cache into the source tree.
function(add_lender_python_test)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "NAME;PATH" "PYTHONPATH;LABELS")

    if(NOT ARG_NAME)
        message(FATAL_ERROR "add_lender_python_test requires NAME")
    endif()
    if(NOT ARG_PATH)
        message(FATAL_ERROR "add_lender_python_test requires PATH")
    endif()
    if(NOT DEFINED STORAGE_LENDER_PYTEST_FOUND)
        execute_process(
            COMMAND ${Python3_EXECUTABLE} -c "import pytest"
            RESULT_VARIABLE _pytest_result
            OUTPUT_QUIET ERROR_QUIET)
        if(_pytest_result EQUAL 0)
            set(STORAGE_LENDER_PYTEST_FOUND TRUE CACHE INTERNAL "pytest is importable")
        else()
            message(FATAL_ERROR "pytest is not importable by ${Python3_EXECUTABLE} (install python3-pytest)")
        endif()
    endif()

    list(JOIN ARG_PYTHONPATH ":" _pythonpath)
    add_test(
        NAME ${ARG_NAME}
        COMMAND ${Python3_EXECUTABLE} -m pytest -q -p no:cacheprovider ${ARG_PATH}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(
        ${ARG_NAME}
        PROPERTIES ENVIRONMENT "PYTHONPATH=${_pythonpath};PYTHONDONTWRITEBYTECODE=1"
                   LABELS "${ARG_LABELS}")
endfunction()
