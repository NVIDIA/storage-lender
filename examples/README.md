# Storage Lender provisioning example

`hello_world.cpp` is a complete C++17 control-plane example. It connects to a local Storage Lender
server, opens one NVMe controller, prints controller and namespace information, creates separate
hugepage-backed CQ and SQ mappings, creates one queue pair, maps the returned BAR0 doorbells, and
tears everything down in dependency order.

It deliberately does not construct or submit an NVMe command and never rings a doorbell. client
application supplies the pilot data path.

## Run the packaged example

The host must already have the pilot kernel, IOMMUFD/VFIO setup, at least two free 2 MiB hugepages,
a running Storage Lender server, socket access, and permission to map the controller's BAR0
resource.

```sh
storage-lender-hello-world 0000:03:00.0
```

Use only a controller dedicated to the pilot and bound to `vfio-pci`.

## Build the installed source

The `storage-lender-examples` package installs this standalone source tree at
`/usr/share/storage-lender/examples/`. With `libstorage-lender-client-dev` installed:

```sh
cmake -S /usr/share/storage-lender/examples -B build-example
cmake --build build-example
./build-example/storage-lender-hello-world 0000:03:00.0
```
