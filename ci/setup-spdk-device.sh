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

SPDK_ROOT="${SPDK_ROOT:-/usr/local/spdk}"
SPDK_SETUP="${SPDK_ROOT}/scripts/setup.sh"

if [[ ! -x "${SPDK_SETUP}" ]]; then
    echo "${SPDK_SETUP} is not executable" >&2
    exit 1
fi

echo "Loading iommufd and wiring /dev/vfio/vfio to /dev/iommu" >&2
sudo modprobe iommufd
sudo mkdir -p /dev/vfio
sudo ln -sf /dev/iommu /dev/vfio/vfio
ls -l /dev/iommu /dev/vfio/vfio >&2

echo "Resetting SPDK device setup" >&2
sudo "${SPDK_SETUP}" reset >&2

echo "Running default SPDK device setup" >&2
sudo "${SPDK_SETUP}" >&2

device_bdf="$(sudo "${SPDK_SETUP}" status | grep -E '^NVMe.+vfio-pci' | head -1 | awk '{print $2}')"

if [[ -z "${device_bdf}" ]]; then
    echo "No NVMe devices found in SPDK setup status output" >&2
    exit 1
fi

echo "Selected NVMe device ${device_bdf}" >&2
printf '%s\n' "${device_bdf}"
