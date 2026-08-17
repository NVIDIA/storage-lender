# Storage Lender roadmap

Storage Lender is experimental. This roadmap communicates direction, not delivery dates or
commitments.

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
