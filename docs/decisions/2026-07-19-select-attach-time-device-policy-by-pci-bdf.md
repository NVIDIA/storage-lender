# Select Attach-Time Device Policy by PCI BDF

**Date:** 2026-07-19 **Status:** Accepted

## Context

SPDK requires `num_io_queues` before controller attach. NVMe serial identity is available only after
attach, so serial-based selection cannot choose that option.

## Decision

Select device policy by canonical PCI BDF. Accept domainless BDFs as domain `0000`, normalize case,
and reject aliases in one configuration.

## Consequences

Policy follows a slot rather than a physical drive. Moving a controller to a different BDF may
select a different policy and requires configuration review. Equivalent client BDF spellings share
one controller identity.

## Related Documentation

- [Device identity and attach-time policy](../Architecture.md#device-identity-and-attach-time-policy)
- [Device policy](../../README.md#device-policy)
