# Contributing to Storage Lender

Storage Lender welcomes focused bug reports, documentation corrections, design proposals, and code
contributions. By participating, you agree to follow the project
[Code of Conduct](CODE_OF_CONDUCT.md).

## Before contributing

Security vulnerabilities must not be reported through a public issue or pull request. Follow the
repository security policy and use NVIDIA PSIRT's private reporting channels.

For non-security work, search existing GitHub issues before opening a new one. Discuss substantial
API, protocol, security-boundary, packaging, or architecture changes with maintainers before
implementation.

Maintainers carefully consider feedback and contributions. Submissions are evaluated for technical
fit, safety, maintainability, compatibility, and project priorities. Submission does not guarantee
acceptance; maintainers may request revisions or additional evidence before deciding.

## Contribution terms

Storage Lender is licensed under Apache-2.0. Unless explicitly stated otherwise, contributions are
submitted under the same license. A Developer Certificate of Origin (DCO) sign-off is the only legal
attestation required for a contribution.

### Signing off your work

We require all contributors to sign off on their commits. This certifies that the contribution is
your original work, that you have the right to submit it under the same license, or that you may
submit it under a compatible license.

Contributions containing commits that are not signed off will not be accepted.

Use the `--signoff` (or `-s`) option when committing:

```sh
git commit --signoff -m "Add cool feature."
```

This appends the following trailer to the commit message:

```text
Signed-off-by: Your Name <your@email.com>
```

The full text of the [Developer Certificate of Origin](https://developercertificate.org/) is:

```text
Developer Certificate of Origin
Version 1.1

Copyright (C) 2004, 2006 The Linux Foundation and its contributors.

Everyone is permitted to copy and distribute verbatim copies of this
license document, but changing it is not allowed.


Developer's Certificate of Origin 1.1

By making a contribution to this project, I certify that:

(a) The contribution was created in whole or in part by me and I
    have the right to submit it under the open source license
    indicated in the file; or

(b) The contribution is based upon previous work that, to the best
    of my knowledge, is covered under an appropriate open source
    license and I have the right under that license to submit that
    work with modifications, whether created in whole or in part
    by me, under the same open source license (unless I am
    permitted to submit under a different license), as indicated
    in the file; or

(c) The contribution was provided directly to me by some other
    person who certified (a), (b) or (c) and I have not modified
    it.

(d) I understand and agree that this project and the contribution
    are public and that a record of the contribution (including all
    personal information I submit with it, including my sign-off) is
    maintained indefinitely and may be redistributed consistent with
    this project or the open source license(s) involved.
```

Preserve third-party notices and identify any third-party material, its source, and its license in
the pull request. Do not submit material you do not have the right to contribute.

## Development setup

The server requires C++23; the public client interface remains C++17-compatible. Install CMake,
Boost, Protobuf, SPDK/DPDK, and a supported compiler before configuring the build.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel "$(nproc)"
ctest --test-dir build --output-on-failure -LE hardware
```

Run repository formatting, linting, and license checks:

```sh
pre-commit run --all-files
./ci/check-spdx.sh
```

Maintainers may run additional project, security, licensing, packaging, and hardware validation
before accepting a change.

Build Debian packages when packaging files change:

```sh
make -f debian/rules debian/control
dpkg-buildpackage -us -uc -b -d
```

## Hardware tests

Do not run hardware tests without an NVMe controller deliberately provisioned for Storage Lender,
with the required IOMMU, VFIO/IOMMUFD, SPDK, hugepage, and sudo configuration. Never use a host boot
or data-bearing controller.

With an approved dedicated device:

```sh
sudo -E env DEVICE_BDF=0000:03:00.0 \
  ctest --test-dir build --output-on-failure -L hardware
```

Absence of provisioned hardware is not a reason to remove, weaken, or silently bypass a hardware
contract.

## Pull requests

Keep changes focused and include:

- the problem and intended behavior;
- tests for behavior changes and documentation for changed contracts;
- security and compatibility impact, especially for DMA, BAR0, protocol, and lifecycle changes;
- third-party provenance when applicable; and
- the commands and results used for verification.

Maintainer and CODEOWNERS approval is required. CI must pass where compatible runners are enabled.
Reviewers may require additional hardware, sanitizer, packaging, security, legal, licensing, or
third-party provenance evidence.
