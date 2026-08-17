# Security policy

## Reporting a vulnerability

**Do not open a public issue, pull request, discussion, or merge request for a security
vulnerability.**

Report potential vulnerabilities to NVIDIA through one of these channels:

- [NVIDIA Security Vulnerability Submission Form](https://www.nvidia.com/object/submit-security-vulnerability.html)
- Email: <psirt@nvidia.com>
- [NVIDIA PSIRT PGP key](https://www.nvidia.com/en-us/security/pgp-key) for encrypted email

Include the affected version or commit, vulnerability type, reproduction steps or proof of concept,
and potential impact. See the
[NVIDIA PSIRT policies](https://www.nvidia.com/en-us/security/psirt-policies/) for the coordinated
disclosure process.

## Supported deployment assumptions

Storage Lender is experimental software for dedicated Linux hosts. Its supported security model
requires:

- an enabled, correctly configured IOMMU and VFIO/IOMMUFD stack;
- an NVMe controller dedicated to the deployment and intentionally bound for userspace access;
- a privileged daemon, normally running as root, with hugepage and device access;
- secure, non-symlinked socket parent directories with restrictive ownership and permissions;
- explicit admission of only trusted local client accounts; and
- client software that follows the documented queue, mapping, and teardown lifecycle.

Storage Lender is not a hostile multi-tenant isolation boundary. Operators must not admit mutually
untrusted clients to one API socket or controller.

## Security boundaries

### Privileged daemon and filesystem admission

The daemon performs privileged DMA registration, controller attachment, and NVMe administration. The
packaged API socket is `/run/storage-lender/api.sock`, owned by `root:storage-lender` with mode
`0660`. Filesystem access is broad API admission: there is no application-level UID/GID allowlist or
per-operation authorization. Any admitted client can invoke every current API operation.

The endpoint implementation validates absolute paths, secure parent ownership and modes, socket
identity, and guarded cleanup. Custom service managers must preserve those constraints and remove
stale socket entries only after verifying the daemon is stopped.

The management socket is separate and defaults to `/run/storage-lender/ctl.sock` as `root:root` with
mode `0600`. Its ownership and mode are its authorization mechanism.

### DMA and IOMMU containment

The daemon accepts one file descriptor through `SCM_RIGHTS`, measures the backing object, requires
stable grow/shrink seals for regular files, validates page-aligned mapping sizes, and registers the
requested range for DMA. Queue creation validates that queue memory fits a mapping owned by the same
session.

The platform IOMMU is the final DMA-containment boundary. A broken or bypassed IOMMU configuration,
incompatible DMA-BUF exporter, or incorrect kernel/VFIO deployment invalidates this model.

### Device policy and direct doorbells

An admitted client may request any syntactically valid, supported PCI BDF. Device policy controls
attach-time queue count and administrative-command timeout; it is not a device authorization list.
Shared and exclusive opens coordinate controller use but do not create tenant isolation.

Clients receive a BAR0 resource path and doorbell offsets and map BAR0 independently. BAR0 exposes
more controller register space than the assigned doorbells, so admitted clients have broad direct
controller access. The daemon is not on the I/O data path and cannot inspect, throttle, retry, or
quiesce client commands.

### Resource quotas

Global and principal quotas limit sessions, descriptors, mappings, mapped bytes, device handles, and
queues. They protect control-plane availability but do not limit IOPS, bandwidth, command rate, or
BAR0 behavior. Peer credentials select a sticky quota principal; they do not add authorization.

### Lifetime and cleanup

Clients must stop submission, drain or otherwise quiesce in-flight I/O, unmap BAR0, delete
submission queues before completion queues, close device handles, and then unmap DMA buffers. The
daemon cannot quiesce the client's direct path.

Backend resources and quota are released only when release is proven. A failed explicit cleanup can
be retried while the session remains active. Unproven disconnect cleanup is charged as orphan usage
and may require operator intervention or daemon restart; a timed-out NVMe administrative command can
still complete later and leaves controller state indeterminate.

## Protocol and implementation scope

Security reports may concern the privileged server, public client library, protobuf framing and
methods, descriptor transfer, DMA registration, controller and queue management, configuration,
socket lifecycle, Debian packaging, systemd integration, or the supplied examples. Frames are
length-prefixed protobuf messages capped at 64 KiB; malformed input is untrusted even though
admitted clients are operationally trusted with the controller.

For the normative component, ownership, execution, and lifecycle model, see
[Storage Lender Architecture](docs/Architecture.md).
