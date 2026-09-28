# storage-lender

A service that lends NVMe I/O queues to unprivileged clients over a Unix domain socket. Clients
obtain DMA-mapped memory, open NVMe controllers, and create their own I/O queues — all without
needing direct VFIO/IOMMU access themselves.

## How it works

Storage Lender separates NVMe provisioning from I/O. The daemon admits clients, imports DMA memory,
opens controllers, and creates queues through SPDK. It then returns queue IDs, a BAR0 resource path,
and doorbell offsets. Clients submit commands, ring mapped doorbells, and poll completions directly;
the daemon is not on that hot path.

Socket access therefore admits a client to a broad trusted boundary rather than an isolated
per-device service. See [Storage Lender Architecture](docs/Architecture.md) for component ownership,
resource lifecycles, failure semantics, and the control/data-path split.

## Dependencies

| Dependency                                                               | Version                                     | How located                                 |
| ------------------------------------------------------------------------ | ------------------------------------------- | ------------------------------------------- |
| [Linux kernel](https://github.com/hdefreitasco/linux-storage-lender.git) | branch `storage-lender-main`                | Host kernel                                 |
| [SPDK](https://github.com/hdefreitasco/spdk-storage-lender.git)          | branch `storage-lender-main`                | `cmake/FindSPDK.cmake` (pkg-config, static) |
| [Boost](https://www.boost.org)                                           | ≥ 1.83                                      | `find_package(Boost REQUIRED ...)`          |
| [Protobuf](https://protobuf.dev)                                         | ≥ 3.21                                      | `find_package(Protobuf REQUIRED)`           |
| [spdlog](https://github.com/gabime/spdlog)                               | 1.17.0                                      | CPM (fetched at configure time)             |
| [toml++](https://github.com/marzer/tomlplusplus)                         | 3.4.0                                       | CPM (fetched at configure time)             |
| [GoogleTest](https://github.com/google/googletest)                       | 1.17.0                                      | CPM (fetched at configure time)             |
| C++ compiler                                                             | Server: C++23; client API: C++17-compatible | GCC ≥ 13 or Clang ≥ 17                      |
| CMake                                                                    | ≥ 3.26                                      | —                                           |

The required Linux and SPDK changes are maintained on the `storage-lender-main` branch of their
linked repositories. SPDK must be built with DPDK (`--with-dpdk`) and installed so that `pkg-config`
can find `spdk_nvme` and `spdk_env_dpdk`. The build links SPDK and DPDK statically.

## Building

```sh
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
```

The server binary is `build/server/storage-lender-server`.

## Development checks

Formatting and shell linting use version-pinned [pre-commit](https://pre-commit.com/) hooks. Install
the orchestrator and repository hook once:

```sh
pipx install pre-commit==4.6.1
pre-commit install --install-hooks
```

The hook formats staged C++, CMake, shell, and Markdown files and runs ShellCheck on staged shell
files. If a formatter changes a file, the commit stops so the changes can be reviewed and staged.
Run every check across the repository with:

```sh
pre-commit run --all-files
```

Standalone formatter installations, such as Homebrew `mdformat`, can support editor integration, but
the hook environments remain authoritative and include the pinned GFM plugin.

## Packaging

Debian packages are built with debhelper 13:

```sh
debian/rules debian/control
dpkg-buildpackage -us -uc -b -d
```

Four binary packages are produced:

| Package                        | Contents                                                                                                                 |
| ------------------------------ | ------------------------------------------------------------------------------------------------------------------------ |
| `storage-lender-server`        | Server binary, `/usr/bin/storage-lender-ctl`, systemd unit, system group, and `/etc/storage-lender/config.toml` conffile |
| `libstorage-lender-client0`    | Shared library (`libstorage_lender_client.so.0`)                                                                         |
| `libstorage-lender-client-dev` | Headers (`/usr/include/storage_lender/`), static archive, `.so` symlink, and CMake config                                |
| `storage-lender-examples`      | Runnable queue-provisioning example and standalone C++ source                                                            |

`libstorage-lender-client-dev` depends on `libstorage-lender-client0` and `libprotobuf-dev`.
`storage-lender-examples` depends on the matching server and client runtime.

Every binary package installs the Storage Lender license, the third-party component manifest, the
open-source notices, and their license texts under `/usr/share/doc/<package-name>/`.

The systemd service (`storage-lender-server.service`) targets `multi-user.target` and restarts on
failure with a 5-second delay, but the Debian package does not enable or start it automatically.
Configure VFIO access and the hugepages required by your SPDK deployment, then start or enable the
service explicitly with `systemctl`.

> **Note:** The build fetches `spdlog` and `toml++` via CPM at configure time. For an air-gapped
> package build, pre-populate `build/_deps/spdlog-src` and `build/_deps/tomlplusplus-src` before
> invoking `dpkg-buildpackage`. Debian and release builds pass both cached roots to CMake
> independently.

### Open-source notice bundle

Create the distributable compliance notice archive from the same configured build used for release
binaries:

```sh
cmake --build build --target NoticeBundle
```

The target needs the source tree corresponding to the linked SPDK build. If CMake cannot infer it
from SPDK's pkg-config paths, configure with `-DSPDK_NOTICE_SOURCE_DIR=/path/to/spdk`.

The archive is written to `build/compliance/storage-lender-<version>-notice-bundle.tar.gz`. It
contains the third-party component manifest, attribution notice, complete referenced license texts,
upstream license files for code incorporated into the server, the Storage Lender license,
build-specific SPDK/DPDK/source metadata, direct shared-library requirements, and SHA-256 checksums.
See [`compliance/README.md`](compliance/README.md) for the release gate and scope.

## Running

The server requires hugepages and VFIO access (typically run as root or with appropriate
capabilities):

```sh
# Bind the NVMe device to vfio-pci first, e.g.:
echo "0000:03:00.0" > /sys/bus/pci/devices/0000:03:00.0/driver/unbind
echo "vfio-pci" > /sys/bus/pci/devices/0000:03:00.0/driver_override
echo "0000:03:00.0" > /sys/bus/pci/drivers/vfio-pci/bind

# When systemd is not starting the daemon, create the configured parent first:
sudo install -d -o root -g storage-lender -m 0750 /run/storage-lender

# Start the server with the repository configuration:
sudo ./build/server/storage-lender-server --config config/storage-lender.toml
```

The packaged server listens on `/run/storage-lender/api.sock` as `root:storage-lender` with mode
`0660`, and on `/run/storage-lender/ctl.sock` as `root:root` with mode `0600`. It shuts down
gracefully on `SIGTERM` or `SIGINT`. systemd creates and removes the `root:storage-lender` `0750`
parent directory; manual runs and custom service managers must manage their configured parent
themselves. The packaged service uses `/etc/storage-lender/config.toml` by default. Every
configuration table and value is optional and overlays one built-in configuration. An empty or
comment-only file is valid. If the selected path does not exist at startup, the server logs a
warning and uses built-in defaults; permission errors, other I/O errors, malformed TOML, unknown
fields, and invalid values remain fatal. The installed example explicitly selects unlimited quota
mode for backward compatibility, while an absent file uses finite enforced quotas.

Grant a trusted client access by adding its user to the packaged socket group:

```sh
sudo usermod --append --groups storage-lender CLIENT_USER
```

Replace `CLIENT_USER` with the client account, then start a new login session before connecting so
the process receives the new supplementary group. Membership grants every current API operation
against every supported PCI BDF; there is no application-level authorization or per-device ACL.
There is no compatibility listener at the former `/tmp` endpoint.

For a manual non-root development run, use a private parent owned by the process:

```sh
socket_dir=$(mktemp -d "${TMPDIR:?TMPDIR must be set}/storage-lender-dev.XXXXXX")
socket_dir=$(realpath "$socket_dir")
printf 'path = "%s/api.sock"\n' "$socket_dir"
```

Copy the printed absolute `path = ".../api.sock"` line literally into the TOML `[sockets.api]` table
and set `owner` and `group` to the names reported for the process by `id -un` and `id -gn`.
Configuration values do not expand `$socket_dir`, `${TMPDIR}`, or other environment variables. An
existing final path makes startup fail and is never removed automatically; after an ungraceful
manual run, verify the daemon is stopped before removing a stale custom socket entry.

`[sockets.api]` and `[sockets.ctl]` are optional field-level overlays. Each configured field
replaces only its built-in default. The API defaults are `/run/storage-lender/api.sock`,
`root:storage-lender`, and `0660`; the ctl defaults are the root-only values shown below.

```toml
[sockets.ctl]
path = "/run/storage-lender/ctl.sock"
owner = "root"
group = "root"
mode = 0o600
```

Both resolved socket settings are restart-only. A `SIGHUP` candidate that changes any API or ctl
socket field is rejected in full and reports that a restart is required.

### Socket troubleshooting

For API startup or connection failures, check all four `[sockets.api]` fields and verify that
`/run/storage-lender` exists as `root:storage-lender` with mode `0750`. Check the connecting
process's current supplementary groups with `id -nG`; editing group membership does not update an
existing session. For ctl failures, also verify the root-only `[sockets.ctl]` setting and invoke the
command through an authorized root context. For custom paths, check for a stale entry left by an
ungraceful manual run. If a reload reports restart-only socket fields, restore the active socket
policy or restart the service with the intended policy.

### Management ctl

`storage-lender-ctl` is a separate, read-only management client. It has no config file and does not
read the server TOML; it uses `/run/storage-lender/ctl.sock` by default or the explicit `--socket`
value. Socket ownership and mode are the only management authorization in this release, so the
packaged default requires root.

```sh
sudo storage-lender-ctl quota
sudo storage-lender-ctl --format json quota
sudo storage-lender-ctl latency
sudo storage-lender-ctl --format json latency
sudo storage-lender-ctl -s /run/storage-lender/ctl.sock -c never quota
```

The command is `storage-lender-ctl [options] quota|latency`. Options may appear before or after the
subcommand, and each option may appear once.

| Short | Long                          | Values                           | Default                        |
| ----- | ----------------------------- | -------------------------------- | ------------------------------ |
| `-s`  | `--socket PATH`               | Absolute management socket path  | `/run/storage-lender/ctl.sock` |
| `-f`  | `--format human\|json`        | Output format                    | `human`                        |
| `-c`  | `--color auto\|always\|never` | Terminal color policy            | `auto`                         |
| `-h`  | `--help`                      | Print usage without connecting   |                                |
| `-V`  | `--version`                   | Print version without connecting |                                |

`quota` human output shows quota-policy generation and mode, then global usage and limits,
default-principal limits, and lexical principal sections. Each section has the fixed resource order
`sessions`, `transferred_fds`, `mapped_buffers`, `max_buffer_bytes`, `mapped_bytes`,
`device_handles`, `completion_queues`, and `submission_queues`. The active and orphan columns show
`-` for `max_buffer_bytes`, which is an admission-size limit rather than accumulated use. Nonzero
orphan usage is amber; a finite limit reached exactly is red. In `auto` mode color requires a
terminal, a non-`dumb` `TERM`, and no `NO_COLOR` setting.

`latency` is a process-wide diagnostic snapshot of parsed API command dispatch. For each known
command, it retains exactly the last 256 completed observations for the daemon process lifetime;
unrecognized method IDs share one bounded `unknown` row. It reports sample and error counts,
one-based peak in-flight count at dispatch start, completion timestamps, and duration p50, p90, p99,
and maximum in microseconds. Non-OK responses remain in the duration percentiles and increment the
error count. Percentiles use nearest rank; with sparse traffic, use the displayed sample count and
prefer maximum over low-sample p99 as tail evidence. All observations reset when the daemon
restarts.

The same rows report p90 and maximum event-loop lag sampled by a one-millisecond heartbeat that is
armed only while an API command is inside dispatch. This helps distinguish yielded backend waits
from a blocked or saturated single event loop without creating steady-state timer wakeups. The
measurement excludes time in the socket backlog, request framing and protobuf parsing, response
serialization and write, later session cleanup, and all direct client NVMe I/O.

`--format json` writes only the selected `GetQuotaStateResponse` or `GetCommandLatencyResponse`
protobuf JSON, without the wire envelope or ANSI escapes. Protobuf JSON quotes unsigned 64-bit
values, such as `"generation":"4"`, so consumers do not lose JavaScript integer precision.

| Exit status | Meaning                                                                                              |
| ----------- | ---------------------------------------------------------------------------------------------------- |
| `0`         | Request and rendering succeeded.                                                                     |
| `1`         | Connection, authorization, framing, malformed response, server status, rendering, or output failure. |
| `2`         | Invalid command syntax or option value.                                                              |

### Logging policy

The optional `[logging]` table selects the process-wide runtime threshold: trace, debug, info, warn,
error, critical, or off. The packaged configuration sets level = "info". If the table is absent,
Debug builds default to trace and all other builds default to info. An empty table or an omitted
`level` field has the same result.

Debug builds contain trace-and-higher statements. Non-Debug builds contain debug-and-higher
statements, so a non-Debug binary rejects level = "trace" rather than silently clamping it. SIGHUP
applies a valid level without dropping active clients; an invalid or unavailable level rejects the
complete candidate configuration and preserves the active policy.

### Device policy

The optional `[devices.default]` table defines the attach-time policy for every NVMe controller.
`num_io_queues` defaults to `65534` and accepts integers from `1` through `65534`;
`admin_command_timeout_ms` defaults to `10000` and accepts integers from `10000` through
`4294967295`. Per-device tables may override either or both fields; each missing field inherits
independently from the resolved device default. An empty per-device table is valid but redundant.

Device table keys and `OpenDevice` accept PCI BDFs case-insensitively as `bb:dd.f` (domain `0000`
implied) or `dddd:bb:dd.f`. The server canonicalizes them to lowercase, domain-qualified form, so
equivalent spellings identify one controller; configuration rejects aliases for the same canonical
BDF.

`num_io_queues` is requested from SPDK before controller attach. The controller may negotiate fewer
queues, and `GetDeviceInfo` reports the effective count. The selected queue count and admin timeout
are fixed for the attachment. Any `[devices]` change requires restart; SIGHUP rejects the complete
candidate configuration instead of partially applying its reloadable fields.

`admin_command_timeout_ms` applies to create and delete CQ/SQ admin commands. A longer timeout
reduces the probability of falsely timing out a slow controller, but it does not make a timed-out
command safe or recover controller state. The command may complete later and leave controller state
that the server does not track.

### Resource governance

`config/storage-lender.toml` is the canonical version-1 schema and fully commented operator example.
Built-in quota mode is `enforced`. Its global defaults are 64 sessions, 256 transferred FDs, 128
mapped buffers, 8 GiB maximum buffer size, 64 GiB mapped bytes, 64 device handles, 512 completion
queues, and 512 submission queues. Default-principal limits are respectively 2, 8, 8, 1 GiB, 2 GiB,
2, 16, and 16. Missing enforced-mode tables and fields inherit these values. The installed example
explicitly sets `mode = "unlimited"`.

`enforced` applies hard global and sticky-principal control-resource ceilings, while `unlimited`
keeps accounting without policy denials.

Quota-denied RPCs return `RESOURCE_EXHAUSTED`, and failed or unproven cleanup stays charged. Session
admission is checked before RPC processing; a denied accepted connection is closed without a status
response, and the rejection may become visible only after `connect()`. These controls are neither
IOPS/bandwidth throttling nor authorization.

SIGHUP atomically installs valid reloadable policy. Current allocations are grandfathered, new
growth uses the new ceilings, principal mapping changes apply only to new sessions, logging changes
apply process-wide, and socket changes require restart. A missing selected file is an invalid reload
and preserves the active policy; startup's `ENOENT` fallback is not applied during reload.

See [resource governance](docs/Architecture.md#resource-governance), the
[API socket and trust boundary](docs/Architecture.md#api-socket-and-trust-boundary),
[management socket and trust boundary](docs/Architecture.md#management-socket-and-trust-boundary),
and the [canonical configuration](config/storage-lender.toml).

## Wire protocol

The shared internal wire contract in [proto/wire.proto](proto/wire.proto) uses a four-byte
little-endian length followed by a protobuf `Request` or `Response`; both directions enforce a 64
KiB frame limit. The client API payloads live in [proto/client.proto](proto/client.proto), and
management payloads in [proto/ctl.proto](proto/ctl.proto). `TransferFd` additionally requires
exactly one `SCM_RIGHTS` descriptor. Responses use gRPC-style status codes, but public
`storage_lender::ClientError` mapping applies only to the client API.

See the [protocol and client boundary](docs/Architecture.md#protocol-and-client-boundary) for layer
ownership and the [client integration guide](docs/external/CLIENT_INTEGRATION.md) for caller
obligations.

## API

Defined in [`proto/client.proto`](proto/client.proto), package `nvidia.storage_lender.v1`.

### Buffer lifecycle

| Method        | Request                      | Response | Description                              |
| ------------- | ---------------------------- | -------- | ---------------------------------------- |
| `TransferFd`  | fd via `SCM_RIGHTS`          | `fd_id`  | Server receives the fd as ancillary data |
| `MapBuffer`   | `fd_id`, `size`, `alignment` | `iova`   | Server IOMMU-registers the buffer        |
| `UnmapBuffer` | `iova`                       | —        | Server IOMMU-unregisters the buffer      |

`TransferFd` accepts non-empty, page-size-aligned buffers. Regular file descriptors must support
file seals and already have `F_SEAL_SHRINK | F_SEAL_GROW`; DMA-BUF descriptors are intrinsically
non-resizable, so the server validates their immutable exported size with `lseek()` and does not
require seals. A typical memfd caller should use
`memfd_create(..., MFD_CLOEXEC | MFD_ALLOW_SEALING)`, `ftruncate()` it to the desired DMA buffer
size rounded up to a system page-size multiple, and then add those seals before transfer. The
server-measured fd size and `MapBuffer.size` must both be multiples of the system page size;
`MapBuffer.size` must also be non-zero and no larger than the server-measured fd size.

### Device lifecycle

| Method          | Request                    | Response                        | Description                                                 |
| --------------- | -------------------------- | ------------------------------- | ----------------------------------------------------------- |
| `OpenDevice`    | `pci_address`, `open_mode` | `device_id`                     | Connect to an NVMe controller                               |
| `CloseDevice`   | `device_id`                | —                               | Detach from the controller after its I/O queues are deleted |
| `GetDeviceInfo` | `device_id`                | model, queue limits, namespaces | Read controller metadata                                    |

`open_mode` is `SHARED` (multiple clients may open the same controller) or `EXCLUSIVE` (single
client only). `pci_address` must be a PCI BDF such as `0000:03:00.0` or `03:00.0`. Each successful
`OpenDevice` returns a unique `device_id` and acquires one reference, including repeated shared
opens by the same client. A `device_id` can be closed exactly once. On disconnect, the server closes
all device IDs still held by that client. `CloseDevice` returns `FAILED_PRECONDITION` while the
client still has a completion or submission queue on that `device_id`; delete submission queues
before completion queues, then close the device.

### Queue lifecycle

| Method                  | Request                                    | Response                | Description                                                      |
| ----------------------- | ------------------------------------------ | ----------------------- | ---------------------------------------------------------------- |
| `CreateCompletionQueue` | `device_id`, `iova`, `queue_size`          | `cq_id`, `cq_db_offset` | Issue NVMe admin `Create I/O CQ`                                 |
| `DeleteCompletionQueue` | `cq_id`                                    | —                       | Issue NVMe admin `Delete I/O CQ` (all SQs must be deleted first) |
| `CreateSubmissionQueue` | `device_id`, `cq_id`, `iova`, `queue_size` | `sq_id`, `sq_db_offset` | Issue NVMe admin `Create I/O SQ`                                 |
| `DeleteSubmissionQueue` | `sq_id`                                    | —                       | Issue NVMe admin `Delete I/O SQ`                                 |

Queue creation requires at least two entries and validates that the requested queue byte footprint
fits inside the mapped buffer: completion queue entries are 16 bytes and submission queue entries
are 64 bytes.

## Client library

`libstorage_lender_client` provides a C++17-compatible API for all server operations without
exposing protobuf or framing to callers. It ships as both a static archive and a shared object.

```cpp
#include "storage_lender/client.hpp"

storage_lender::StorageLenderClient client;
if (auto err = storage_lender::StorageLenderClient::connect(
        "/run/storage-lender/api.sock", client);
    err != storage_lender::ClientError::OK) {
    // Handle connection error.
}

uint32_t device_id;
if (auto err = client.open_device(
        "0000:03:00.0", storage_lender::OpenDeviceMode::SHARED, device_id);
    err != storage_lender::ClientError::OK) {
    // Handle open error.
}

storage_lender::QueueInfo cq;
if (auto err = client.create_cq(device_id, iova, 64, cq);
    err != storage_lender::ClientError::OK) {
    // Handle CQ creation error.
}

storage_lender::QueueInfo sq;
if (auto err = client.create_sq(device_id, cq.qid, iova, 64, sq);
    err != storage_lender::ClientError::OK) {
    // Handle SQ creation error.
}
// sq.db_offset is the doorbell offset into mmap()ed BAR0 from pci_resource_path.
```

Methods return `storage_lender::ClientError`; successful calls write returned values into output
parameters, and `ClientError::OK` indicates success. Link with:

```cmake
find_package(StorageLenderClient REQUIRED)

target_link_libraries(my_app PRIVATE StorageLender::Client::Static)
# or
target_link_libraries(my_app PRIVATE StorageLender::Client::Shared)
```

`find_package(StorageLenderClient)` is provided by `libstorage-lender-client-dev` and automatically
pulls in the Protobuf dependency.

## Testing

```sh
cmake --build build --parallel
ctest --test-dir build --output-on-failure -LE hardware
```

Tests are component-local and grouped by layer. Server tests live under
`server/tests/{unit,integration,hardware}`, while public client API tests live under
`client/tests/integration`. See the
[build and test architecture](docs/Architecture.md#build-and-test-architecture) for production and
test-target composition.

CTest labels select the useful cross-component views:

```sh
ctest --test-dir build -L unit --output-on-failure
ctest --test-dir build -L integration --output-on-failure
ctest --test-dir build -L server --output-on-failure
ctest --test-dir build -L client --output-on-failure
```

Representative direct build targets include:

| Test binary                                | Covers                                                 |
| ------------------------------------------ | ------------------------------------------------------ |
| `StorageLenderServerBufferManagerTest`     | buffer-manager unit behavior using `MockNvmeBackend`   |
| `StorageLenderServerBufferIntegrationTest` | fd transfer and buffer RPCs over a Unix socket         |
| `StorageLenderServerQuotaIntegrationTest`  | quota enforcement through the server protocol          |
| `StorageLenderClientApiIntegrationTest`    | public `StorageLenderClient` API against a live server |
| `StorageLenderServerSpdkBackendTest`       | real SPDK backend; labeled `server;hardware`           |

`StorageLenderServerSpdkBackendTest` skips before initializing SPDK unless `DEVICE_BDF` identifies
an explicitly provisioned NVMe controller. Do not run the `hardware` label against an unprepared
device.

## Continuous integration

The repository includes GitHub Actions workflows for formatting, shell linting, SPDX, build,
non-hardware test, sanitizer, coverage, and Debian package jobs. Jobs remain disabled until
maintainers configure compatible organization runners and set the repository CI enablement variable.
The SPDX check requires notices on project source and build files and requires NVIDIA copyright
ranges to end in the current year.

SPDK is expected to be available on the runner. `cmake/FindSPDK.cmake` defaults to
`/usr/local/spdk`.

The protected hardware workflow uses `ci/setup-spdk-device.sh` to reset SPDK device binding, select
the first NVMe device configured for `vfio-pci`, export `DEVICE_BDF`, and run tests labeled
`hardware` with `sudo -E`. It requires a dedicated, explicitly provisioned self-hosted runner and
never runs pull-request code.

### Coverage report

Requires `gcovr` (`sudo apt install gcovr`). Configure with `ENABLE_COVERAGE=ON` (this automatically
enables `BUILD_TESTS`), build normally, then run the `Coverage` target:

```sh
cmake -B build-cov -DENABLE_COVERAGE=ON
cmake --build build-cov -j$(nproc)
cmake --build build-cov --target Coverage
```

The target runs all tests and writes an HTML report to `build-cov/coverage/index.html`. A summary is
also printed to the terminal:

```
lines:     XX.X% (NNN out of NNN)
functions: XX.X% (NN out of NN)
branches:  XX.X% (NNN out of NNN)
```

## Documentation

- **Architecture:** [current system architecture](docs/Architecture.md) and
  [consequential decisions](docs/decisions/README.md).
- **Operator and client integration:**
  [client integration guide](docs/external/CLIENT_INTEGRATION.md).
- **Community:** [contribution guide](CONTRIBUTING.md), [governance](GOVERNANCE.md), and
  [support](SUPPORT.md). Vulnerability reporting is documented in the repository security policy.
- **Compliance:** [compliance guidance](compliance/README.md).

## License

Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

Storage Lender is licensed under the Apache License 2.0. See [`LICENSE`](LICENSE) for the complete
terms. Each Debian binary package installs the project license and third-party open-source notices
under `/usr/share/doc/<package-name>/`.

## Project layout

| Path                 | Responsibility                                                          |
| -------------------- | ----------------------------------------------------------------------- |
| `proto/`             | Shared protobuf wire messages                                           |
| `server/`            | Control-plane daemon, resource managers, SPDK backend, and server tests |
| `client/`            | C++17-compatible public client library and API integration tests        |
| `config/`, `debian/` | Runtime policy, service integration, and packaging                      |
| `examples/`          | Small client provisioning examples                                      |
| `docs/`              | Architecture, decisions, roadmap, and external integration guidance     |
| `compliance/`        | Distribution notices and third-party license material                   |
