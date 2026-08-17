# Serialize Control-Plane State on One Event-Loop Thread

**Date:** 2026-07-16 **Status:** Accepted

## Context

Each admitted client runs in a stackful coroutine that can yield during socket I/O, timer waits, and
other asynchronous operations. Session resources are per-client, but `DeviceManager` and
`QuotaManager` coordinate state shared by all sessions. Their validation, accounting, and mutation
sequences need a defined execution model to remain atomic without internal synchronization.

## Decision

Run the production `boost::asio::io_context` on one thread. Accept handling, client coroutines,
manager calls, quota acquisition/release, usage snapshots, and configuration replacement execute on
that event-loop thread. `QuotaManager` acquisition, release, and policy replacement, and synchronous
shared `DeviceManager` mutations, do not yield between validation, accounting, and state mutation.

`QueueManager` is session-local. Its CQ/SQ admin operations may yield through the timer-backed sleep
function while polling for completion, allowing other sessions to run. Queue quota is reserved
before creation enters backend polling and remains associated with that session-local manager
through deletion polling.

Do not add manager mutexes for hypothetical concurrent handlers. Before adding another
`io_context::run()` thread or moving control-plane work to other threads, serialize shared state
with an Asio strand or redesign affected components with explicit synchronization and document the
new model.

## Consequences

- Non-yielding `QuotaManager` mutations and synchronous shared `DeviceManager` mutations are atomic
  with respect to other handlers without manager-level locks.
- Queue-admin polling may interleave with other sessions, but the in-flight `QueueManager` state is
  session-local and its quota is already reserved.
- Handlers interleave cooperatively at yield points rather than running in parallel, so blocking
  event-loop work delays all control-plane progress.
- Introducing control-plane parallelism has an explicit migration cost: every affected shared
  component must gain serialized access or documented synchronization before the execution model
  changes.

## Related Documentation

- [Process and execution model](../Architecture.md#process-and-execution-model)
- [Production event-loop construction and run](../../server/src/main.cpp)
