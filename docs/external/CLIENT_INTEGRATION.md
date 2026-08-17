# Client integration

Reference for the non-obvious contracts of the C++17 Storage Lender client library. A client
application supplies the NVMe data path. The `storage-lender-examples` package contains a runnable
provisioning example; it does not submit NVMe commands.

Shared ownership and lifecycle terms are defined in
[Storage Lender Architecture](../Architecture.md).

## API contract

One `StorageLenderClient` instance represents one session. Calls on an instance must be serialized;
IDs and IOVAs are session-local.

Connect to the packaged endpoint at `/run/storage-lender/api.sock`, or to the exact absolute path
selected by the operator. There is no `/tmp` compatibility listener or client fallback. The
connecting process must already have the configured socket group in its current supplementary
groups; a new login is required after group admission changes.

For the packaged `root:storage-lender` `0660` endpoint, group membership grants every current API
operation against every supported PCI BDF. Storage Lender has no application-level authorization or
per-device ACL. Socket path and permission changes require a server restart rather than `SIGHUP`.

## Integration sequence

```text
connect -> open device -> read DeviceInfo -> allocate and map CQ/SQ memory
        -> create CQ -> create SQ -> map BAR0 -> run data path -> quiesce -> teardown
```

## Buffer contract

For a regular file or `memfd`:

| Property       | Requirement                                                          |
| -------------- | -------------------------------------------------------------------- |
| Backing length | Non-zero and a multiple of the host system page size                 |
| File seals     | `F_SEAL_SHRINK \| F_SEAL_GROW` before `transfer_fd()`                |
| Mapping length | Non-zero, system-page aligned, and no larger than the backing object |
| Lifetime       | Until all queues or commands that reference the mapping are quiesced |

The reference host-memory path uses a memfd created with `MFD_HUGETLB | MFD_ALLOW_SEALING`. Its
backing allocation is rounded to the hugepage size; the DMA mapping need only cover the
system-page-rounded queue bytes. The host must have enough free hugepages for the backing
allocation.

`DeviceInfo::page_size` is the controller page size. The control plane validates mapping lengths
against the host system page size.

A partner DMA-BUF path may use GPU memory, pinned host memory, `udmabuf`, or another approved
exporter. The descriptor must expose a stable, seekable size and be importable by the supplied
kernel's IOMMUFD path. Exporter ownership and coherency remain data-path responsibilities.

`map_buffer()` accepts an `alignment` argument, but release 0.1.0 does not enforce it. A successful
`map_buffer()` consumes its `fd_id`. After `transfer_fd()` succeeds, the caller may close its copy
of the descriptor.

## Device and queue contract

`SHARED` permits other shared opens. `EXCLUSIVE` requires sole ownership and blocks subsequent opens
until close.

| Constraint                | Value                                           |
| ------------------------- | ----------------------------------------------- |
| Queue depth               | `2 <= entries <= DeviceInfo::max_queue_entries` |
| CQ storage                | At least `entries * 16` bytes                   |
| SQ storage                | At least `entries * 64` bytes                   |
| Queue mappings            | Separate base IOVAs for CQ and SQ               |
| Creation order            | CQ, then SQ                                     |
| Current association model | One SQ per CQ; the SQ uses its CQ's queue ID    |

## BAR0 and data-path boundary

The library returns `DeviceInfo::pci_resource_path` and queue doorbell offsets. The consuming
process must map enough BAR0 space to cover the offsets.

Storage Lender handles the control plane: sessions, DMA imports, controller opens, and queue
administration. The consuming data path handles:

- NVMe commands and PRP/SGL construction;
- queue head, tail, and completion-phase state;
- memory ordering and cache coherency;
- doorbell writes and completion polling;
- namespace range validation; and
- in-flight I/O recovery.

BAR0 access exposes more than the assigned doorbells. Processes with BAR0 access are inside the
trusted pilot boundary.

## Teardown

After quiescing I/O:

```text
unmap BAR0 -> delete_sq -> delete_cq -> close_device -> unmap_buffer
```

The server attempts session cleanup when the connection closes. It cannot quiesce a client data
path. Calling `unmap_buffer()` while a queue or command still references the mapping is unsafe even
if the call succeeds.

## Errors

| Error                 | Meaning                                                                                                 |
| --------------------- | ------------------------------------------------------------------------------------------------------- |
| `NOT_FOUND`           | Unknown session resource or unavailable BDF                                                             |
| `INVALID_ARGUMENT`    | Invalid BDF, buffer, mapping length, or queue parameters                                                |
| `PERMISSION_DENIED`   | Shared/exclusive open conflict                                                                          |
| `RESOURCE_EXHAUSTED`  | A quota-controlled RPC exceeded a principal or daemon-wide limit                                        |
| `DEADLINE_EXCEEDED`   | Operation timed out                                                                                     |
| `FAILED_PRECONDITION` | Resource dependency or teardown order violation                                                         |
| `INTERNAL`            | Server-side DMA import, SPDK, or admin-command failure                                                  |
| `IO_ERROR`            | Socket or frame send/receive failure, including an oversized frame; session state is no longer reliable |
| `PROTOCOL_ERROR`      | Malformed Protocol Buffer response or operation payload                                                 |

Session admission is quota-controlled before RPC processing. A denied accepted connection is closed
without a status response, so `connect()` may succeed and the rejection may become visible only when
the connection is used. The client library suppresses `SIGPIPE` internally, so the rejection
surfaces as an `IO_ERROR` on first use rather than as a signal; consult the server journal for the
quota-denial diagnostic.

The library does not expose the server's textual error. If the daemon terminates mid-session, the
next client call returns `ClientError::IO_ERROR`; because the client suppresses `SIGPIPE`
internally, a consumer needs no `SIGPIPE` handler of its own. After `IO_ERROR` or `PROTOCOL_ERROR`,
quiesce the data path and re-establish state from scratch — reconnect, re-open devices, re-map
buffers, and re-create queues. See
[resource ownership and lifecycle](../Architecture.md#resource-ownership-and-lifecycle) for
diagnostics context.
