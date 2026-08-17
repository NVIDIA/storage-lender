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

add_library(StorageLenderWireFramingObjects OBJECT src/framing.cpp)
set_target_properties(
    StorageLenderWireFramingObjects
    PROPERTIES POSITION_INDEPENDENT_CODE ON
               CXX_STANDARD 17
               CXX_STANDARD_REQUIRED ON
               CXX_EXTENSIONS OFF)
target_include_directories(StorageLenderWireFramingObjects
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(StorageLenderWireFraming STATIC $<TARGET_OBJECTS:StorageLenderWireFramingObjects>)
add_library(StorageLender::Wire::Framing ALIAS StorageLenderWireFraming)
add_dependencies(StorageLenderWireFraming CheckNoExceptions)
target_include_directories(StorageLenderWireFraming PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(
    StorageLenderWireFraming
    PROPERTIES CXX_STANDARD 17
               CXX_STANDARD_REQUIRED ON
               CXX_EXTENSIONS OFF)

if(BUILD_TESTS)
    add_lender_test(
        NAME
        StorageLenderWireFramingTest
        SOURCES
        tests/framing_test.cpp
        LIBRARIES
        StorageLender::Wire::Framing
        LABELS
        unit)
endif()
