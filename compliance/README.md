# Storage Lender notice bundle

This directory is the source for the distributable third-party open-source notice bundle. It covers
the components declared in `manifest.tsv` and keeps their license texts beside the attribution
notice.

The bundle does not license Storage Lender itself. The Storage Lender license is copied from the
repository's top-level `LICENSE` file into every generated bundle as `LICENSE.storage-lender`.

## Create a release bundle

After configuring the project, build the `NoticeBundle` target:

```sh
cmake --build build --target NoticeBundle
```

The target needs the SPDK source tree, including its initialized `dpdk` and `isa-l` directories.
CMake normally infers that tree from SPDK's pkg-config library paths. If it cannot, configure with
`-DSPDK_NOTICE_SOURCE_DIR=/path/to/spdk`.

The target writes a reproducible archive to:

```text
build/compliance/storage-lender-<version>-notice-bundle.tar.gz
```

The archive contains this notice, the component manifest, all referenced license texts, the Storage
Lender license, build-specific component metadata, and SHA-256 checksums. The target depends on the
server, ctl, and public client binaries so its build metadata also records each direct
shared-library requirement.

## Release gate

For each release, the release owner must:

1. Build the archive in the same clean environment as the release binaries.
1. Confirm that `BUILD-METADATA.txt` identifies the intended SPDK, DPDK, and ISA-L sources, reports
   clean project and SPDK source states, and has no `unknown` value for an incorporated component.
1. Compare the direct shared libraries in `BUILD-METADATA.txt` with the packaged files. Libraries
   merely required from the target operating system are not redistributed by the Debian packages;
   any library copied into a different distribution format must be added to the manifest and
   notices.
1. Review changes to `CMakeLists.txt`, `FindSPDK.cmake`, and the package payload for new or removed
   third-party components.
1. Ship the notice archive with release artifacts and keep a copy with the release approval record.

This is engineering release material, not a substitute for the organization's open-source review and
legal approval.
