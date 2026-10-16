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

# storage-lender-ctl is a python3 package (see pyproject.toml). CMake's part is to generate the
# protocol bindings, assemble an installable copy of the package in the build tree, run its tests
# under CTest, and install it.

# The package version is declared once in _version.py; keep it equal to the CMake project version.
file(STRINGS src/storage_lender_ctl/_version.py PYTHON_CTL_VERSION_LINE REGEX "^__version__ = ")
string(REGEX REPLACE "^__version__ = \"(.*)\"$" "\\1" PYTHON_CTL_VERSION "${PYTHON_CTL_VERSION_LINE}")
if(NOT PYTHON_CTL_VERSION STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR "ctl_py/src/storage_lender_ctl/_version.py declares ${PYTHON_CTL_VERSION} "
                        "but the project version is ${PROJECT_VERSION}")
endif()

# Staged layout mirrors the installed one: <stage>/share/storage-lender/python holds the package.
set(PYTHON_CTL_STAGE_DIR ${CMAKE_CURRENT_BINARY_DIR}/stage)
set(PYTHON_CTL_SITE_DIR ${PYTHON_CTL_STAGE_DIR}/share/storage-lender/python)
set(PYTHON_CTL_PACKAGE_DIR ${PYTHON_CTL_SITE_DIR}/storage_lender_ctl)

file(MAKE_DIRECTORY ${PYTHON_CTL_PACKAGE_DIR}/proto)
protobuf_generate(
    LANGUAGE python
    PROTOS ${CMAKE_SOURCE_DIR}/proto/wire.proto ${CMAKE_SOURCE_DIR}/proto/ctl.proto
    IMPORT_DIRS ${CMAKE_SOURCE_DIR}/proto
    PROTOC_OUT_DIR ${PYTHON_CTL_PACKAGE_DIR}/proto
    OUT_VAR PYTHON_CTL_GENERATED)

set(PYTHON_CTL_SOURCES src/storage_lender_ctl/__init__.py src/storage_lender_ctl/_version.py
                       src/storage_lender_ctl/proto/__init__.py)

set(PYTHON_CTL_STAGED ${PYTHON_CTL_GENERATED})
foreach(SOURCE IN LISTS PYTHON_CTL_SOURCES)
    string(REPLACE "src/" "" RELATIVE_PATH ${SOURCE})
    set(STAGED ${PYTHON_CTL_SITE_DIR}/${RELATIVE_PATH})
    add_custom_command(
        OUTPUT ${STAGED}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE} ${STAGED}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}
        COMMENT "Staging ${SOURCE}"
        VERBATIM)
    list(APPEND PYTHON_CTL_STAGED ${STAGED})
endforeach()

add_custom_target(StorageLenderCtlPy ALL DEPENDS ${PYTHON_CTL_STAGED})

if(BUILD_TESTS)
    add_lender_python_test(
        NAME
        StorageLenderCtlPyTests
        PATH
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        PYTHONPATH
        ${PYTHON_CTL_SITE_DIR}
        LABELS
        ctl
        unit)
endif()
