#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -euo pipefail

usage() {
    echo "usage: $0 BUILD_DIR SPDK_ROOT SPDLOG_ROOT TOMLPLUSPLUS_ROOT SERVER_BINARY CTL_BINARY CLIENT_LIBRARY [OUTPUT_DIR]" >&2
}

if (($# < 7 || $# > 8)); then
    usage
    exit 2
fi

readonly BUILD_DIR="$1"
readonly SPDK_ROOT="$2"
readonly SPDLOG_ROOT="$3"
readonly TOMLPLUSPLUS_ROOT="$4"
readonly SERVER_BINARY="$5"
readonly CTL_BINARY="$6"
readonly CLIENT_LIBRARY="$7"
readonly OUTPUT_DIR="${8:-${BUILD_DIR}/compliance}"

repo_root="$(git rev-parse --show-toplevel)"
readonly repo_root
readonly compliance_dir="${repo_root}/compliance"
readonly manifest="${compliance_dir}/manifest.tsv"

require_file() {
    if [[ ! -f "$1" ]]; then
        echo "notice bundle: required file not found: $1" >&2
        exit 1
    fi
}

require_file "${repo_root}/LICENSE"
require_file "${compliance_dir}/README.md"
require_file "${compliance_dir}/THIRD_PARTY_NOTICES.md"
require_file "${manifest}"
require_file "${SPDK_ROOT}/LICENSE"
require_file "${SPDK_ROOT}/include/spdk/version.h"
require_file "${SPDK_ROOT}/dpdk/VERSION"
require_file "${SPDK_ROOT}/dpdk/license/README"
require_file "${SPDK_ROOT}/isa-l/LICENSE"
require_file "${SPDLOG_ROOT}/LICENSE"
require_file "${SPDLOG_ROOT}/include/spdlog/fmt/bundled/base.h"
require_file "${SPDLOG_ROOT}/include/spdlog/fmt/bundled/fmt.license.rst"
require_file "${TOMLPLUSPLUS_ROOT}/LICENSE"
require_file "${TOMLPLUSPLUS_ROOT}/include/toml++/impl/version.hpp"
require_file "${SERVER_BINARY}"
require_file "${CTL_BINARY}"
require_file "${CLIENT_LIBRARY}"

if ! awk -F '\t' '
    NR == 1 {
        expected = "component\tversion\trelationship\tdistributed_in\tlicense_expression\tlicense_files\tsource"
        if ($0 != expected) {
            print "notice bundle: invalid manifest header" > "/dev/stderr"
            failed = 1
        }
        next
    }
    NF != 7 {
        print "notice bundle: manifest line " NR " has " NF " fields; expected 7" > "/dev/stderr"
        failed = 1
    }
    {
        for (i = 1; i <= NF; ++i) {
            if ($i == "") {
                print "notice bundle: manifest line " NR " has an empty field" > "/dev/stderr"
                failed = 1
            }
        }
    }
    END { exit failed }
' "${manifest}"; then
    exit 1
fi

while IFS= read -r license_path; do
    require_file "${compliance_dir}/${license_path}"
done < <(awk -F '\t' 'NR > 1 { print $6 }' "${manifest}" | tr ',' '\n' | sort -u)

project_version="$(awk '
    /VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+/ {
        for (i = 1; i <= NF; ++i) {
            if ($i == "VERSION") {
                print $(i + 1)
                exit
            }
        }
    }
' "${repo_root}/CMakeLists.txt")"
if [[ -z "${project_version}" ]]; then
    echo "notice bundle: could not determine the project version" >&2
    exit 1
fi

read_define() {
    local name="$1"
    local file="$2"
    awk -v name="${name}" '$1 == "#define" && $2 == name { print $3; exit }' "${file}"
}

readonly spdk_version_header="${SPDK_ROOT}/include/spdk/version.h"
spdk_major="$(read_define SPDK_VERSION_MAJOR "${spdk_version_header}")"
spdk_minor="$(read_define SPDK_VERSION_MINOR "${spdk_version_header}")"
spdk_patch="$(read_define SPDK_VERSION_PATCH "${spdk_version_header}")"
if [[ -z "${spdk_major}" || -z "${spdk_minor}" || -z "${spdk_patch}" ]]; then
    echo "notice bundle: could not determine the SPDK version" >&2
    exit 1
fi
spdk_version="${spdk_major}.$(printf '%02d' "${spdk_minor}")"
if ((spdk_patch != 0)); then
    spdk_version="${spdk_version}.${spdk_patch}"
fi
readonly spdk_version

dpdk_version="$(tr -d '[:space:]' <"${SPDK_ROOT}/dpdk/VERSION")"
fmt_version_number="$(read_define FMT_VERSION "${SPDLOG_ROOT}/include/spdlog/fmt/bundled/base.h")"
if [[ -z "${dpdk_version}" || -z "${fmt_version_number}" ]]; then
    echo "notice bundle: could not determine the DPDK or fmt version" >&2
    exit 1
fi
printf -v fmt_version '%d.%d.%d' \
    "$((fmt_version_number / 10000))" \
    "$(((fmt_version_number / 100) % 100))" \
    "$((fmt_version_number % 100))"

manifest_version() {
    local component="$1"
    awk -F '\t' -v component="${component}" '$1 == component { print $2; exit }' "${manifest}"
}

assert_manifest_version() {
    local component="$1"
    local actual_version="$2"
    if [[ "$(manifest_version "${component}")" != "${actual_version}" ]]; then
        echo "notice bundle: ${component} version in manifest does not match its source" >&2
        exit 1
    fi
}

spdlog_version_header="${SPDLOG_ROOT}/include/spdlog/version.h"
require_file "${spdlog_version_header}"
printf -v spdlog_version '%d.%d.%d' \
    "$(read_define SPDLOG_VER_MAJOR "${spdlog_version_header}")" \
    "$(read_define SPDLOG_VER_MINOR "${spdlog_version_header}")" \
    "$(read_define SPDLOG_VER_PATCH "${spdlog_version_header}")"

assert_manifest_version spdlog "${spdlog_version}"
assert_manifest_version fmt "${fmt_version}"

tomlplusplus_version_header="${TOMLPLUSPLUS_ROOT}/include/toml++/impl/version.hpp"
printf -v tomlplusplus_version '%d.%d.%d' \
    "$(read_define TOML_LIB_MAJOR "${tomlplusplus_version_header}")" \
    "$(read_define TOML_LIB_MINOR "${tomlplusplus_version_header}")" \
    "$(read_define TOML_LIB_PATCH "${tomlplusplus_version_header}")"
assert_manifest_version toml++ "${tomlplusplus_version}"

cpm_version="$(awk '
    /set\(CPM_DOWNLOAD_VERSION/ {
        value = $2
        sub(/\).*/, "", value)
        print value
        exit
    }
' "${repo_root}/cmake/CPM.cmake")"
googletest_version="$(awk '
    /cpmaddpackage\("gh:google\/googletest@/ {
        value = $0
        sub(/.*@/, "", value)
        sub(/"\).*/, "", value)
        print value
        exit
    }
' "${repo_root}/CMakeLists.txt")"
assert_manifest_version CPM.cmake "${cpm_version}"
assert_manifest_version GoogleTest "${googletest_version}"

cache_value() {
    local name="$1"
    awk -F '=' -v name="${name}" '$1 ~ "^" name ":" { print $2; exit }' \
        "${BUILD_DIR}/CMakeCache.txt"
}

pkg_config_version() {
    pkg-config --modversion "$1" 2>/dev/null || echo unknown
}

boost_version="unknown"
boost_include_dir="$(cache_value Boost_INCLUDE_DIR)"
if [[ -f "${boost_include_dir}/boost/version.hpp" ]]; then
    boost_version="$(awk -F '"' '/BOOST_LIB_VERSION/ { version = $2; gsub("_", ".", version); print version; exit }' \
        "${boost_include_dir}/boost/version.hpp")"
else
    boost_config_dir="$(cache_value Boost_DIR)"
    if [[ "${boost_config_dir}" =~ Boost-([0-9]+\.[0-9]+\.[0-9]+)$ ]]; then
        boost_version="${BASH_REMATCH[1]}"
    fi
fi

protobuf_version="unknown"
protoc="$(cache_value Protobuf_PROTOC_EXECUTABLE)"
if [[ -x "${protoc}" ]]; then
    protobuf_version="$(${protoc} --version | awk '{ print $NF }')"
fi

systemd_version="$(pkg_config_version libsystemd)"
openssl_version="$(pkg_config_version openssl)"
libuuid_version="$(pkg_config_version uuid)"
libnuma_version="$(pkg_config_version numa)"
readonly systemd_version openssl_version libuuid_version libnuma_version

spdk_revision="unknown"
spdk_source_object="unknown"
dpdk_source_object="unknown"
isal_source_object="unknown"
spdk_source_state="unknown"
if git -C "${SPDK_ROOT}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    spdk_revision="$(git -C "${SPDK_ROOT}" rev-parse HEAD)"
    spdk_source_object="$(git -C "${SPDK_ROOT}" rev-parse 'HEAD^{tree}')"
    dpdk_source_object="$(git -C "${SPDK_ROOT}" rev-parse HEAD:dpdk 2>/dev/null || true)"
    isal_source_object="$(git -C "${SPDK_ROOT}" rev-parse HEAD:isa-l 2>/dev/null || true)"
    dpdk_source_object="${dpdk_source_object:-unknown}"
    isal_source_object="${isal_source_object:-unknown}"
    spdk_source_state="clean"
    if [[ -n "$(git -C "${SPDK_ROOT}" status --porcelain --untracked-files=no)" ]]; then
        spdk_source_state="modified"
    fi
fi

project_revision="unknown"
project_source_state="unknown"
if git -C "${repo_root}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    project_revision="$(git -C "${repo_root}" rev-parse HEAD)"
    project_source_state="clean"
    if [[ -n "$(git -C "${repo_root}" status --porcelain --untracked-files=all)" ]]; then
        project_source_state="modified"
    fi
fi

mkdir -p "${OUTPUT_DIR}"
work_dir="$(mktemp -d "${OUTPUT_DIR}/.notice-bundle.XXXXXX")"
trap 'rm -rf "${work_dir}"' EXIT

readonly bundle_name="storage-lender-${project_version}-notice-bundle"
readonly bundle_dir="${work_dir}/${bundle_name}"
mkdir -p "${bundle_dir}/licenses" "${bundle_dir}/upstream/DPDK-license"

cp "${repo_root}/LICENSE" "${bundle_dir}/LICENSE.storage-lender"
cp "${compliance_dir}/README.md" "${bundle_dir}/README.md"
cp "${compliance_dir}/THIRD_PARTY_NOTICES.md" "${bundle_dir}/THIRD_PARTY_NOTICES.md"
cp "${manifest}" "${bundle_dir}/manifest.tsv"
cp "${compliance_dir}"/licenses/* "${bundle_dir}/licenses/"

cp "${SPDK_ROOT}/LICENSE" "${bundle_dir}/upstream/SPDK-LICENSE"
cp "${SPDK_ROOT}/dpdk/license/README" "${bundle_dir}/upstream/DPDK-license/README"
cp "${SPDK_ROOT}"/dpdk/license/*.txt "${bundle_dir}/upstream/DPDK-license/"
if [[ -f "${SPDK_ROOT}/dpdk/license/Linux-syscall-note" ]]; then
    cp "${SPDK_ROOT}/dpdk/license/Linux-syscall-note" \
        "${bundle_dir}/upstream/DPDK-license/Linux-syscall-note"
fi
cp "${SPDK_ROOT}/isa-l/LICENSE" "${bundle_dir}/upstream/ISA-L-LICENSE"
cp "${SPDLOG_ROOT}/LICENSE" "${bundle_dir}/upstream/spdlog-LICENSE"
cp "${TOMLPLUSPLUS_ROOT}/LICENSE" "${bundle_dir}/upstream/tomlplusplus-LICENSE"
cp "${SPDLOG_ROOT}/include/spdlog/fmt/bundled/fmt.license.rst" \
    "${bundle_dir}/upstream/fmt-LICENSE"

direct_dependencies() {
    local binary="$1"
    readelf -d "${binary}" | awk -F '[][]' '/NEEDED/ { print $2 }' | sort -u
}

{
    echo "Storage Lender release notice metadata"
    echo
    echo "Project version: ${project_version}"
    echo "Project source revision: ${project_revision}"
    echo "Project source state: ${project_source_state}"
    echo "SPDK version: ${spdk_version}"
    echo "SPDK source revision: ${spdk_revision}"
    echo "SPDK source tree object: ${spdk_source_object}"
    echo "SPDK source state: ${spdk_source_state}"
    echo "DPDK version: ${dpdk_version}"
    echo "DPDK source object: ${dpdk_source_object}"
    echo "ISA-L source object: ${isal_source_object}"
    echo "spdlog version: ${spdlog_version}"
    echo "toml++ version: ${tomlplusplus_version}"
    echo "fmt version: ${fmt_version}"
    echo "Boost version: ${boost_version}"
    echo "Protocol Buffers version: ${protobuf_version}"
    echo "systemd libsystemd version: ${systemd_version}"
    echo "OpenSSL version: ${openssl_version}"
    echo "util-linux libuuid version: ${libuuid_version}"
    echo "numactl libnuma version: ${libnuma_version}"
    echo
    echo "Direct shared libraries required by storage-lender-server:"
    direct_dependencies "${SERVER_BINARY}" | sed 's/^/  /'
    echo
    echo "Direct shared libraries required by storage-lender-ctl:"
    direct_dependencies "${CTL_BINARY}" | sed 's/^/  /'
    echo
    echo "Direct shared libraries required by libstorage_lender_client:"
    direct_dependencies "${CLIENT_LIBRARY}" | sed 's/^/  /'
} >"${bundle_dir}/BUILD-METADATA.txt"

(
    cd "${bundle_dir}"
    find . -type f -print0 \
        | sort -z \
        | xargs -0 sha256sum \
        | sed 's|  \./|  |' >"${work_dir}/CHECKSUMS.sha256.tmp"
    mv "${work_dir}/CHECKSUMS.sha256.tmp" CHECKSUMS.sha256
)

readonly archive="${OUTPUT_DIR}/${bundle_name}.tar.gz"
readonly temporary_archive="${archive}.tmp"
readonly source_date_epoch="${SOURCE_DATE_EPOCH:-0}"

tar --sort=name --format=ustar --mtime="@${source_date_epoch}" \
    --owner=0 --group=0 --numeric-owner \
    -C "${work_dir}" -cf - "${bundle_name}" \
    | gzip -n >"${temporary_archive}"
mv "${temporary_archive}" "${archive}"

echo "Created ${archive}"
