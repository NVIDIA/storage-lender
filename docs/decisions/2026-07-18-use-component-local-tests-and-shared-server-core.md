# Use Component-Local Tests and Shared Server Core

**Date:** 2026-07-18 **Status:** Accepted

## Context

The former root test tree separated tests from the components that owned the behavior. Server tests
repeatedly compiled production implementation sources, and a monolithic integration suite mixed
server protocol behavior, public client behavior, reusable fixtures, and hardware-dependent
coverage. That structure obscured ownership and allowed production and test composition to drift.

## Decision

Keep tests under the component that owns the behavior, divided there by `unit`, `integration`, and
`hardware` layers. Build the daemon implementation without `main.cpp` as
`StorageLender::Server::Core`; link both production and server tests to that target.

Keep reusable server-boundary infrastructure in test-only `StorageLender::Server::TestSupport`.
Register focused executables with explicit sources and orthogonal component/layer CTest labels. Keep
SPDK and hardware calls behind `NvmeBackend`; normal tests use `MockNvmeBackend`, while hardware
tests require explicit device provisioning.

## Consequences

- Production and server tests share one composition of the daemon implementation rather than
  compiling parallel source lists.
- With `BUILD_TESTS=OFF`, test support is absent from the build and production targets retain no
  test-only seams.
- Explicit test sources require deliberate maintenance when files are added or removed, avoiding the
  implicit behavior and reconfiguration hazards of globbing.
- Component and layer labels remain independently selectable, while hardware coverage stays opt-in
  and explicitly provisioned.

## Related Documentation

- [Build and test architecture](../Architecture.md#build-and-test-architecture)
- [Server target composition](../../server/CMakeLists.txt)
