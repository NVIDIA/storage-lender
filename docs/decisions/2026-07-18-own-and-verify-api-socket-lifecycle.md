# Own and Verify the API Socket Lifecycle

**Date:** 2026-07-18 **Status:** Accepted

## Context

The daemon can run with enough privilege that unconditionally unlinking a configured pathname could
remove an entry it does not own. If admitted clients can write the socket's parent, they can replace
or redirect the final entry during creation or cleanup. The pathname, parent hierarchy, and bound
socket identity therefore form security state rather than incidental startup detail.

## Decision

Treat the final Unix socket pathname and parent hierarchy as daemon-owned security state. Require an
absolute path and validate the hierarchy without following symlinks. Reject a parent writable by
admitted clients. Bind only an absent final pathname, apply configured ownership and mode, verify
the socket, and listen only after verification succeeds.

Retain the parent descriptor and the socket's device/inode. On failure or shutdown, unlink only when
the final entry still identifies that socket. Treat socket path, owner, group, and mode as
restart-only configuration; reject a SIGHUP candidate that changes any of them.

## Consequences

- Startup fails on stale or replaced final entries instead of deleting them or assuming ownership.
- The packaged service manager owns creation and removal of the configured parent directory with the
  required metadata.
- Filesystem admission remains a broad trust boundary: endpoint hardening does not add
  application-level or per-device authorization.

## Related Documentation

- [API socket and trust boundary](../Architecture.md#api-socket-and-trust-boundary)
- [Unix socket endpoint ownership interface](../../server/include/unix_socket_endpoint.hpp)
