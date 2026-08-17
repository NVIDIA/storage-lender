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

readonly PROJECT_LICENSE="Apache-2.0"
readonly THIRD_PARTY_LICENSE="MIT"
readonly COMMENT_PREFIX='^[[:space:]]*(#|//|/\*|\*)[[:space:]]*'
readonly -a APACHE_LICENSE_MARKERS=(
    'Licensed under the Apache License, Version 2.0'
    'http://www.apache.org/licenses/LICENSE-2.0'
    'limitations under the License.'
)

END_YEAR="$(date -u +%Y)"
readonly END_YEAR

repo_root="$(git rev-parse --show-toplevel)"
cd "${repo_root}"

is_checked_file() {
    case "$1" in
        .gitlab-ci.yml | CMakeLists.txt | */CMakeLists.txt | debian/rules | \
            *.c | *.cc | *.cpp | *.cxx | *.h | *.hh | *.hpp | *.hxx | \
            *.proto | *.cmake | *.cmake.in | *.sh)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

is_third_party_file() {
    [[ "$1" == "cmake/CPM.cmake" ]]
}

checked=0
failed=0
license_pattern="${COMMENT_PREFIX}SPDX-License-Identifier:[[:space:]]*[^[:space:]]+[[:space:]]*$"
project_license_pattern="${COMMENT_PREFIX}SPDX-License-Identifier:[[:space:]]*${PROJECT_LICENSE}[[:space:]]*$"
third_party_license_pattern="${COMMENT_PREFIX}SPDX-License-Identifier:[[:space:]]*${THIRD_PARTY_LICENSE}[[:space:]]*$"
copyright_pattern="${COMMENT_PREFIX}SPDX-FileCopyrightText:[[:space:]]*Copyright \\(c\\) ([0-9]{4}-)?${END_YEAR} NVIDIA CORPORATION & AFFILIATES\\. All rights reserved\\.[[:space:]]*$"
copyright_slug_pattern="${COMMENT_PREFIX}SPDX-FileCopyrightText:"

while IFS= read -r -d '' file; do
    if ! is_checked_file "${file}"; then
        continue
    fi

    checked=$((checked + 1))

    if ! grep -Eq "${license_pattern}" "${file}"; then
        echo "${file}: missing SPDX-License-Identifier slug" >&2
        failed=$((failed + 1))
        continue
    fi

    if is_third_party_file "${file}"; then
        if ! grep -Eq "${third_party_license_pattern}" "${file}"; then
            echo "${file}: expected third-party SPDX license slug ${THIRD_PARTY_LICENSE}" >&2
            failed=$((failed + 1))
        fi
        if ! grep -Eq "${copyright_slug_pattern}" "${file}"; then
            echo "${file}: missing SPDX-FileCopyrightText" >&2
            failed=$((failed + 1))
        fi
        continue
    fi

    if ! grep -Eq "${project_license_pattern}" "${file}"; then
        echo "${file}: expected SPDX license slug ${PROJECT_LICENSE}" >&2
        failed=$((failed + 1))
    fi

    if ! grep -Eq "${copyright_pattern}" "${file}"; then
        echo "${file}: NVIDIA SPDX copyright must end in ${END_YEAR}" >&2
        failed=$((failed + 1))
    fi

    normalized_text="$(
        sed -E 's@^[[:space:]]*(#|//|/\*|\*/|\*)[[:space:]]?@@' "${file}" \
            | tr '\n\r\t' '   ' \
            | tr -s ' '
    )"
    for marker in "${APACHE_LICENSE_MARKERS[@]}"; do
        if [[ "${normalized_text}" != *"${marker}"* ]]; then
            echo "${file}: missing Apache-2.0 boilerplate marker: ${marker}" >&2
            failed=$((failed + 1))
        fi
    done
done < <(git ls-files -z)

if ((failed > 0)); then
    echo "SPDX check failed with ${failed} error(s) across ${checked} checked files" >&2
    exit 1
fi

echo "SPDX check passed for ${checked} files (copyright end year ${END_YEAR})"
