# Architectural Decisions

These records preserve consequential choices that constrain future changes. Current behavior is
normative in [Storage Lender Architecture](../Architecture.md) and the source; Git history retains
completed implementation detail.

New records are named `<date>-<slug>.md` (for example
`2026-07-20-report-deadline-without-controller-reset.md`) so decisions authored on concurrent
branches never collide on a shared sequence number. Order the table below by date.

| Decision                                                                                                                             | Date       | Status   |
| ------------------------------------------------------------------------------------------------------------------------------------ | ---------- | -------- |
| [Serialize control-plane state on one event-loop thread](2026-07-16-serialize-control-plane-state-on-one-event-loop-thread.md)       | 2026-07-16 | Accepted |
| [Retain charges for unproven cleanup](2026-07-16-retain-charges-for-unproven-cleanup.md)                                             | 2026-07-16 | Accepted |
| [Own and verify the API socket lifecycle](2026-07-18-own-and-verify-api-socket-lifecycle.md)                                         | 2026-07-18 | Accepted |
| [Use component-local tests and shared `StorageLender::Server::Core`](2026-07-18-use-component-local-tests-and-shared-server-core.md) | 2026-07-18 | Accepted |
| [Select attach-time device policy by PCI BDF](2026-07-19-select-attach-time-device-policy-by-pci-bdf.md)                             | 2026-07-19 | Accepted |
