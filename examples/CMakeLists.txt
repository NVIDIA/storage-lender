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
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.26)
    project(storage-lender-examples LANGUAGES CXX)
    include(GNUInstallDirs)
    find_package(StorageLenderClient REQUIRED)
endif()

add_executable(StorageLenderHelloWorld hello_world.cpp)
if(TARGET CheckNoExceptions)
    add_dependencies(StorageLenderHelloWorld CheckNoExceptions)
endif()
target_link_libraries(StorageLenderHelloWorld PRIVATE StorageLender::Client::Shared)
set_target_properties(
    StorageLenderHelloWorld
    PROPERTIES CXX_STANDARD 17
               CXX_STANDARD_REQUIRED ON
               CXX_EXTENSIONS OFF
               OUTPUT_NAME storage-lender-hello-world)

install(TARGETS StorageLenderHelloWorld RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES CMakeLists.txt hello_world.cpp README.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/storage-lender/examples)
