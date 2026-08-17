# Retain Charges for Unproven Cleanup

**Date:** 2026-07-16 **Status:** Accepted

## Context

Quota accounting represents resources that may still exist in the kernel, SPDK, or hardware. A
backend error does not prove that the corresponding resource was released. Explicit operations can
retain session bookkeeping for a retry, but disconnect cleanup must eventually destroy that
bookkeeping even when release remains unproven.

## Decision

Attach each quota lease to the resource entry whose lifetime it measures. Release the lease only
after the underlying resource has been successfully released. A failed explicit deletion keeps both
the entry and charge so the client can retry. If disconnect cleanup cannot prove release before
destroying session state, mark the lease orphaned and retain its charge until restart.

Clean up dependencies in this order: submission queues, completion queues, device handles, mapped
buffers, unused transferred descriptors, and the session. Continue cleanup after failures while
preserving every unproven charge.

## Consequences

- Failed or orphaned cleanup can reduce available capacity until a later retry succeeds or the
  daemon restarts.
- Usage snapshots distinguish active totals from orphan totals at principal and global scope, while
  both remain relevant to admission.
- The daemon avoids silent overcommit by never returning capacity for a resource whose release it
  cannot prove.

## Related Documentation

- [Resource ownership and lifecycle](../Architecture.md#resource-ownership-and-lifecycle)
- [Quota lease and usage accounting interfaces](../../server/include/quota_manager.hpp)
