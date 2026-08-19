# Storage Lender roadmap

Storage Lender is experimental. This roadmap communicates direction, not delivery dates or
commitments.

## File Lending PoC

**Status: Experimental.** File lending is an early proof of concept and is not supported for
production use.

We are developing a proof of concept for lending storage backed by regular files. The server
coordinates with a storage system to keep a file and its LBA mapping immutable for the lease
lifetime. It then uses NVMe SR-IOV, shared namespaces, and namespace attachment to give the client
direct access to the backing namespace through a secondary controller. A richer set of access
controls may be added over time, including exclusive write access. We are also evaluating
block-level enforcement, and a proposed NVMe extension for LBA range protection. Until device-side
enforcement is available, the PoC assumes a trusted client.

The initial PoC targets XFS. Future enhancements may support a broadening set of file and storage
systems, including user-space filesystems.

**Data corruption risk:** Until device-side LBA range protection is available, out-of-range client
writes, or races between filesystem and raw device I/O, can corrupt the filesystem. Use the PoC only
with dedicated, disposable test storage.

## Current focus

- Validate the privileged control plane and direct NVMe data path on explicitly provisioned
  hardware.
- Strengthen device admission, BAR0 exposure boundaries, and operator diagnostics.
- Improve configuration, quota visibility, cleanup observability, and recovery guidance.
- Stabilize the C++ client API and packaging contracts through broader integration feedback.
- Establish reproducible open-source CI and release evidence.

## Candidate follow-up work

- Per-device and per-operation authorization policy.
- More restricted doorbell mapping where supported by the platform.
- Controller reset, asynchronous-event, hot-remove, and in-flight recovery handling.
- Readiness, inventory, metrics, alerting, and administrative lifecycle operations.
- Additional compatibility coverage across kernels, SPDK versions, DMA-BUF exporters, and hardware.
- Explicit API compatibility and release-support policy after the experimental phase.

Priorities may change based on security findings, hardware validation, maintainer capacity, and user
feedback. Proposals should start as GitHub issues and follow
[`CONTRIBUTING.md`](../CONTRIBUTING.md).
