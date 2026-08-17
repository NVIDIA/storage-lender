# Third-party open-source notices

Storage Lender includes or uses the open-source components listed below. The corresponding license
texts are in `licenses/`, and the machine-readable release inventory is `manifest.tsv`. These
notices apply only to the named third-party components. Storage Lender's own terms are in
`LICENSE.storage-lender`.

Component versions marked `build-resolved` are recorded in `BUILD-METADATA.txt` when the release
bundle is generated.

## Components incorporated into distributed material

### CPM.cmake 0.42.1

- Relationship: vendored in the source distribution as `cmake/CPM.cmake`.
- License: MIT.
- Copyright: Copyright (c) 2019-2023 Lars Melchior and contributors.
- Source: <https://github.com/cpm-cmake/CPM.cmake>

### SPDK

- Relationship: selected SPDK libraries are statically linked into the server.
- Licenses: BSD-3-Clause and BSD-2-Clause.
- Copyright holders represented in the selected SPDK libraries include Intel Corporation, 6WIND
  S.A., NXP, IBM Corporation, Broadcom, Mellanox Technologies, Western Digital Corporation, NVIDIA
  Corporation & Affiliates, Red Hat, Samsung Electronics, Nutanix, Oracle, NetApp, the Regents of
  the University of California, and individual contributors.
- Source: <https://github.com/spdk/spdk>

SPDK records file-level attribution and license exceptions with SPDX headers. The exact source
revision used for a release remains the authoritative record for the statically linked files.

### DPDK

- Relationship: selected DPDK libraries and drivers are statically linked into the server through
  SPDK's environment library.
- Licenses used by the selected user-space code: BSD-3-Clause, BSD-2-Clause, ISC, and MIT. Files
  offered as BSD-3-Clause or GPL-2.0-only are used under the BSD-3-Clause alternative.
- Copyright holders represented in the selected DPDK libraries include Intel Corporation, 6WIND
  S.A., Arm Limited, Mellanox Technologies, NXP, Cavium, Red Hat, IBM Corporation, NVIDIA
  Corporation & Affiliates, the NetBSD Foundation, and individual contributors.
- Source: <https://github.com/DPDK/dpdk>

DPDK records file-level attribution and license choices with SPDX headers. The exact source revision
used for a release remains the authoritative record for the statically linked files.

### ISA-L

- Relationship: statically linked into the server through SPDK.
- License: BSD-3-Clause.
- Copyright: Copyright (c) 2011-2024 Intel Corporation. All rights reserved.
- Source: <https://github.com/intel/isa-l>

### spdlog 1.17.0

- Relationship: compiled into the server.
- License: MIT.
- Copyright: Copyright (c) 2016-present, Gabi Melman and spdlog contributors.
- Source: <https://github.com/gabime/spdlog>

### toml++ 3.4.0

- Relationship: header-only source compiled into the server.
- License: MIT.
- Copyright: Copyright (c) Mark Gillard and contributors.
- Source: <https://github.com/marzer/tomlplusplus>

### fmt 12.1.0

- Relationship: the copy bundled by spdlog is compiled into the server.
- License: MIT, including fmt's optional object-code notice exception.
- Copyright: Copyright (c) 2012-present, Victor Zverovich and fmt contributors.
- Source: <https://github.com/fmtlib/fmt>

## Direct shared-library dependencies not bundled in Debian packages

The Debian packages declare these as operating-system dependencies; they do not copy the libraries
into the package. Their licenses are included here for attribution and to make the notice bundle
reusable for release review. If a different distribution format bundles any of these libraries, its
release owner must add the exact upstream notices for the bundled build.

Compiler runtime, C/C++ runtime, dynamic-loader, and other operating-system libraries may also
appear in `BUILD-METADATA.txt`. They are supplied by the target operating system rather than
declared or redistributed by this project, so they are outside this component manifest. A
self-contained distribution that copies them must inventory their licenses separately.

- Boost — BSL-1.0 — <https://www.boost.org>
- Protocol Buffers — BSD-3-Clause — <https://github.com/protocolbuffers/protobuf>
- systemd libsystemd — LGPL-2.1-or-later — <https://github.com/systemd/systemd>
- OpenSSL — Apache-2.0 for OpenSSL 3.x — <https://github.com/openssl/openssl>
- util-linux libuuid — BSD-3-Clause — <https://github.com/util-linux/util-linux>
- numactl libnuma — LGPL-2.1-only — <https://github.com/numactl/numactl>

## Build and test dependency not distributed

GoogleTest 1.17.0 is fetched only when tests are enabled and is not installed or included in release
binaries. It is licensed under BSD-3-Clause and is available from
<https://github.com/google/googletest>.
