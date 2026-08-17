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
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)

if(NOT DEFINED SPDK_ROOT)
    set(SPDK_ROOT "/usr/local/spdk")
endif()
set(ENV{PKG_CONFIG_PATH} "${SPDK_ROOT}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")

# pkg_check_modules always populates both <prefix>_* (dynamic) and <prefix>_STATIC_* (static, from
# --static) variable sets.
pkg_check_modules(SPDK REQUIRED spdk_nvme spdk_env_dpdk)

# The installed SPDK prefix does not always retain the source license files. Locate the
# corresponding source tree for release notice generation, while allowing packagers to provide it
# explicitly.
if(NOT DEFINED SPDK_NOTICE_SOURCE_DIR)
    set(_spdk_notice_source_dir "${SPDK_ROOT}")
    if(NOT EXISTS "${_spdk_notice_source_dir}/LICENSE"
       OR NOT EXISTS "${_spdk_notice_source_dir}/dpdk/license/README")
        unset(_spdk_notice_source_dir)
        foreach(_library_dir ${SPDK_STATIC_LIBRARY_DIRS})
            get_filename_component(_candidate "${_library_dir}/../../.." ABSOLUTE)
            if(EXISTS "${_candidate}/LICENSE" AND EXISTS "${_candidate}/dpdk/license/README")
                set(_spdk_notice_source_dir "${_candidate}")
                break()
            endif()
        endforeach()
    endif()
    set(SPDK_NOTICE_SOURCE_DIR
        "${_spdk_notice_source_dir}"
        CACHE PATH "SPDK source tree used to collect release license notices")
endif()

# SPDK and DPDK static archives register subsystems and drivers via constructors and weak symbols,
# so the linker must include all their object files regardless of whether a symbol is referenced
# (--whole-archive). System libraries (.so or thin archives) do not require this treatment.
#
# Partition SPDK_STATIC_LIBRARIES into two buckets by probing SPDK_ROOT/lib for a corresponding .a
# file. Anything found there goes under --whole-archive; everything else (pthread, dl, ...) is
# linked normally.
set(_spdk_whole_libs)
set(_spdk_other_libs)
foreach(lib ${SPDK_STATIC_LIBRARIES})
    find_library(
        _lib_path
        NAMES lib${lib}.a
        HINTS ${SPDK_STATIC_LIBRARY_DIRS}
        NO_DEFAULT_PATH)
    if(_lib_path)
        list(APPEND _spdk_whole_libs "${_lib_path}")
    else()
        list(APPEND _spdk_other_libs "-l${lib}")
    endif()
    unset(_lib_path CACHE)
endforeach()

# libisal (ISA-L) is bundled with SPDK and provides CRC and XOR primitives used by libspdk_util. It
# is not listed in the pkg-config output so it must be found and added explicitly.
find_library(
    SPDK_ISAL_LIB
    NAMES libisal.a
    HINTS ${SPDK_STATIC_LIBRARY_DIRS}
    NO_DEFAULT_PATH REQUIRED)

add_library(SPDK::nvme INTERFACE IMPORTED)
target_include_directories(SPDK::nvme INTERFACE ${SPDK_STATIC_INCLUDE_DIRS})
target_link_libraries(
    SPDK::nvme
    INTERFACE -Wl,--whole-archive
              ${_spdk_whole_libs}
              -Wl,--no-whole-archive
              ${_spdk_other_libs}
              ${SPDK_ISAL_LIB}
              OpenSSL::SSL
              OpenSSL::Crypto
              -luuid
              -lnuma)
target_link_options(SPDK::nvme INTERFACE ${SPDK_STATIC_LDFLAGS_OTHER})
target_compile_options(SPDK::nvme INTERFACE ${SPDK_STATIC_CFLAGS_OTHER})
