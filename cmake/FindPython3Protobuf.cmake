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

#[=======================================================================[.rst:
FindPython3Protobuf
-------------------

Finds the ``google.protobuf`` runtime for the interpreter selected by ``find_package(Python3)``.
Generated ``_pb2.py`` modules require a runtime at least as new as the ``protoc`` that produced
them, so callers should request the minimum version they generate against.

Result variables:

``Python3Protobuf_FOUND``
``Python3Protobuf_VERSION``
#]=======================================================================]

execute_process(
    COMMAND ${Python3_EXECUTABLE} -c "import google.protobuf; print(google.protobuf.__version__)"
    RESULT_VARIABLE _python3_protobuf_result
    OUTPUT_VARIABLE _python3_protobuf_output
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)

if(_python3_protobuf_result EQUAL 0)
    set(Python3Protobuf_VERSION "${_python3_protobuf_output}")
else()
    unset(Python3Protobuf_VERSION)
endif()
unset(_python3_protobuf_result)
unset(_python3_protobuf_output)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
    Python3Protobuf
    REQUIRED_VARS Python3Protobuf_VERSION
    VERSION_VAR Python3Protobuf_VERSION
    REASON_FAILURE_MESSAGE
        "the google.protobuf runtime is not importable by ${Python3_EXECUTABLE} (install python3-protobuf)")
