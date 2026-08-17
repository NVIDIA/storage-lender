# Storage Lender deployment and integration

Storage Lender is an experimental privileged Linux service that provisions NVMe I/O queues and DMA
mappings for trusted local client applications. The service owns the control plane; clients submit
I/O directly to the controller.

## Documents

- [Client integration contract](CLIENT_INTEGRATION.md)
- [Architecture and trust boundaries](../Architecture.md)
- [Canonical configuration](../../config/storage-lender.toml)

## Terminology

| Term    | Meaning                                                                  |
| ------- | ------------------------------------------------------------------------ |
| BDF     | Domain-qualified PCI address, for example `0000:03:00.0`                 |
| Session | One client connection and its server-owned resources                     |
| IOVA    | Controller-visible I/O virtual address returned by `map_buffer()`        |
| SQ / CQ | NVMe submission queue / completion queue                                 |
| BAR0    | Controller PCI region containing registers and doorbells                 |
| IOMMUFD | Kernel interface used by the supported SPDK path to import client memory |

## Prerequisites

Use a dedicated evaluation host and expendable NVMe device. The host requires:

- Linux with IOMMU, VFIO, IOMMUFD, and hugepage support appropriate for the selected SPDK build;
- SPDK built with DPDK and installed where `pkg-config` can locate it;
- an NVMe controller that is not used by mounted filesystems, swap, RAID, LVM, or host boot; and
- a trusted local account for each client application.

The exact kernel, SPDK, DMA-BUF exporter, and hardware combination must be qualified by the
operator. Storage Lender does not make an incompatible DMA path safe.

## Build and package

Build from source:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel "$(nproc)"
ctest --test-dir build --output-on-failure -LE hardware
```

Build Debian packages:

```sh
make -f debian/rules debian/control
dpkg-buildpackage -us -uc -b -d
```

The packages provide the server, client runtime and development files, management CLI, and
provisioning example. Each package installs the Apache-2.0 project license and third-party notices.

## Prepare the host

Configure an IOMMU, allocate hugepages for SPDK, and bind only the selected expendable controller to
`vfio-pci`. Confirm the final driver and IOMMU group before starting the service. SPDK's
`scripts/setup.sh` may assist with hugepage and device setup, but operators remain responsible for
verifying that no host workload uses the controller.

Example inspection and binding sequence:

```sh
BDF=0000:03:00.0
lspci -Dnnk -s "$BDF"
echo vfio-pci | sudo tee "/sys/bus/pci/devices/$BDF/driver_override"
echo "$BDF" | sudo tee "/sys/bus/pci/devices/$BDF/driver/unbind"
echo "$BDF" | sudo tee /sys/bus/pci/drivers/vfio-pci/bind
lspci -Dnnk -s "$BDF"
```

## Install and start

From the package output directory:

```sh
sudo apt install \
  ./storage-lender-server_*.deb \
  ./libstorage-lender-client0_*.deb \
  ./storage-lender-examples_*.deb
sudoedit /etc/storage-lender/config.toml
sudo systemctl enable --now storage-lender-server.service
```

The installed configuration is a fully commented example. Every table and field is optional. The
installed example explicitly selects unlimited quota mode for compatibility; an absent file uses
finite enforced built-in quotas. Invalid syntax, unknown fields, permission errors, and invalid
values fail startup.

Add each trusted client account to the API socket group, then start a new login session:

```sh
sudo usermod --append --groups storage-lender CLIENT_USER
```

Membership grants every current API operation against every supported PCI BDF. There is no
application-level authorization or per-device ACL.

## Configuration and reload

The packaged API endpoint is `/run/storage-lender/api.sock` as `root:storage-lender` with mode
`0660`. The management endpoint is `/run/storage-lender/ctl.sock` as `root:root` with mode `0600`.
Systemd creates the secure API parent directory. Manual runs and custom service managers must create
and validate their own secure parent and handle stale final paths.

Logging, quota limits, and principal mapping can be reloaded with `SIGHUP` when the complete
candidate configuration is valid. Socket and attach-time device-policy changes require restart and
cause the entire reload candidate to be rejected. Existing allocations are grandfathered; new growth
uses the new policy.

See [`config/storage-lender.toml`](../../config/storage-lender.toml) for all schema fields and
built-in defaults.

## Verify provisioning

```sh
storage-lender-hello-world "$BDF"
sudo storage-lender-ctl quota
sudo storage-lender-ctl latency
```

The example validates connection, discovery, DMA queue-memory import, CQ/SQ creation, BAR0 mapping,
and teardown. It deliberately does not submit NVMe commands. The client application owns NVMe
command construction, queue state, doorbell writes, completion polling, and data-path recovery.

## Trust and deployment boundary

- Admit only trusted local clients; socket access is broad control-plane admission.
- Treat BAR0 mapping as broad controller access, not isolated doorbell access.
- Keep the selected controller isolated from host storage and other workloads.
- Use the IOMMU as the final DMA containment boundary.
- Quiesce direct I/O before queue, controller, or DMA teardown.
- Recreate the entire session after connection loss or daemon restart.
- Treat resource quotas as availability controls, not I/O throttling or authorization.

Vulnerability reporting and the complete supported security model are documented in the repository
security policy.

## Troubleshooting

Start with:

```sh
systemctl status storage-lender-server.service
sudo journalctl -u storage-lender-server.service -b --no-pager
```

Common checks include the resolved socket ownership and mode, the client's current supplementary
groups, hugepage availability, IOMMUFD and VFIO state, the selected controller's driver, canonical
BDF spelling, queue sizes and creation order, mapping ownership and page alignment, quota state, and
stale socket paths. An ordinary sealed memfd can pass control-plane validation and still fail DMA
import; use a DMA-BUF or hugepage path supported by the deployed kernel and SPDK stack.

## Experimental limitations

The current release has no hostile-tenant isolation, per-device authorization, restricted BAR0
mapping, transparent reconnect, session replay, I/O throttling, controller reset recovery, or
persistent state. Hardware tests require an explicitly provisioned controller and must not run on an
ordinary shared runner.
