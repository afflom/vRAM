# Using vRAM on Linux

## Virtualized Linux workstation

The `vram-machine` command uses stock QEMU, Linux FUSE and the Verdant page store. Install `qemu-system-x86` and `fuse3` alongside the package. Use an existing bootable VM image and the firmware settings it expects. The command runs in the foreground and owns its private runtime directory.

```sh
vram-machine run --state "$XDG_RUNTIME_DIR/workstation" --ram-mib 4096 --add-mib 4096 -- \
  -accel kvm -drive file=workstation.qcow2,format=qcow2
```

Run `vram-machine grow --state "$XDG_RUNTIME_DIR/workstation"` in a second terminal. Linux applications use the additional online System RAM through normal allocations. Check `lsmem` and `/proc/meminfo` inside the guest. The Linux boot option `memhp_default_state=online` selects automatic onlining. To have the entire capacity available from boot, set `--ram-mib 8192` and omit `--add-mib`.

The launcher reserves the configured total plus 4 KiB in private unlinked files before boot. Unique dense literal bases use those files. Choose a local disk with sufficient free space using `--backing-dir DIRECTORY` or use the default user cache. Active pages, kernel cache and model metadata consume hypervisor RAM. Shut down the guest normally to release the temporary file and mount. The guest's VM disk holds persistent data. Like other volatile memory, this device loses its contents if the memory server or machine stops.

### Size a virtualized workstation to its configured maximum

The installed launcher uses the lower of the firmware maximum and the configured guest ceiling as its default RAM target. On systemd hosts the package publishes the SMBIOS type 16 total at boot so a regular user can read it. Use the domain XML for the OS you intend to boot. Its `maxMemory` sets the guest ceiling, with `memory` as the fallback. You can also supply a documented platform limit with `--platform-max-mib` or a QEMU and guest OS limit with `--guest-max-mib`. An unknown limit is reported as `null` and requires configuration before a maximum-size boot.

```sh
vram-machine capacity --domain-xml workstation.xml
vram-machine run --state "$XDG_RUNTIME_DIR/workstation" --domain-xml workstation.xml -- \
  -accel kvm -drive file=workstation.qcow2,format=qcow2
```

`capacity` reports the target, the required backing bytes, the planned files and the byte shortfall. The default backing directory is the user cache. To pool disks, repeat `--backing-dir` with an existing directory on each filesystem, using the same options for planning, qualification and launch. The launcher counts each filesystem once and splits reserves at the operating system's per-file limit. It starts QEMU only after securing the full target plus one 4 KiB replacement page. The process address-space limit, actual quotas and QEMU's own mappings also affect admission. `--add-mib` reserves part of the total for hotplug. A numeric `--ram-mib` explicitly sets boot memory.

To measure this configuration before boot, run the following command while the backing space is free. It temporarily allocates the entire reserve, writes distinct dense pages, verifies every byte and then releases the files. The optional rates below are example requirements in MiB/s. The command prints a JSON receipt and exits unsuccessfully if either floor is missed.

```sh
vram-machine qualify --domain-xml workstation.xml --min-read-mib-s 100 --min-write-mib-s 50
```

Rates measure calls to the Verdant store for 64-page sequential requests during this run. The receipt also gives the slowest observed request in each direction. Guest scheduling, changes in storage load, and other access patterns affect application performance.

## Directly booted Linux swap

The package installs an amd64 server and a systemd service template. The swap service is enabled explicitly. Use a Linux kernel with NBD, swap and 4 KiB pages. Choose an unused whole NBD device. An occupied device fails attachment.

1. Check whether Linux zswap is active with `cat /sys/module/zswap/parameters/enabled`. vRAM can start when this reads N or the parameter is absent. If you choose to switch from zswap, use your distribution's boot configuration to set `zswap.enabled=0` and reboot. Existing ordinary swap devices can stay enabled.
2. Start vRAM with `sudo systemctl enable --now vram@nbd0.service`. This loads NBD, attaches the Verdant store and activates swap at priority 100. The service reports readiness after swapon succeeds.
3. Run your applications normally. Check `swapon --show` and `systemctl status vram@nbd0.service`. Run `sudo systemctl kill --kill-whom=main --signal=USR1 vram@nbd0.service` then `journalctl -u vram@nbd0.service -n 8` to see live store bytes, literal allocations, exact reuse, fast writes and dense residual captures.
4. To drain the device and disable automatic startup, use `sudo systemctl disable --now vram@nbd0.service`. If Linux needs more memory to complete swapoff, close applications or provide another swap device. The service keeps serving pages while it retries.

The default logical size is one eighth of Linux MemAvailable at startup, rounded down to MiB and capped at 1 TiB. Only touched groups allocate page metadata. Edit `/etc/default/vram` to set `VRAM_SIZE_MIB` explicitly or change `VRAM_SEARCH_NODES` (0–49). Apply settings with an orderly service restart. Allow room for literal payloads, the applications' resident pages, allocator overhead and the kernel. RAM-only swap shares physical capacity with applications.

Keep the serving process alive while its swap is active. Power loss, SIGKILL or a server crash can lose paged-out data. Use a disk-backed swap device for hibernation. The service never writes page payloads to disk itself.

Source, verification receipts and QEMU guest RAM instructions are at https://github.com/afflom/vRAM.

## Guest projection port

Add `--projection` before the QEMU options in the workstation example, reserve a DIMM with `--add-mib`, and run `vram-machine grow` to attach it. The grow response gives the guest physical base address and size of the DIMM. The launcher adds a virtio-serial port named `org.verdant.project.1`. Linux with virtio console support and udev normally exposes it as `/dev/virtio-ports/org.verdant.project.1`. In a minimal guest, match that name in `/sys/class/virtio-ports/*/name` and open the corresponding `/dev/vport*`.

Give the port to a single trusted guest process or kernel service. Its authority covers **every page on that VM's reserved DIMM**. The protocol takes an aligned byte offset relative to the DIMM base. A guest service must own the operand, keep its physical location stable, and exclude concurrent writes to it until the reply completes. The host broker flushes the DIMM inode before each accepted request. Ordinary resident CPU access continues to work while the port is idle.

The package supplies `/usr/include/vram/projection.h`. Use the header when building the guest caller. For example, given an open port, a stable DIMM offset, and 64 signed row weights:

```c
struct vram_request q = {.operation = VRAM_PROJECT, .offset = dimm_offset};
memcpy(q.weights, weights, sizeof q.weights);
int64_t columns[64];
int route = vram_projection_call(port_fd, &q, columns);
```

Column `c` equals the sum over rows `r` of `weights[r] * page[64*r+c]`, with unsigned byte input and signed 64-bit accumulation. Return value 1 identifies the packed Verdant contraction, including palette exceptions. Return value 0 identifies another exact store route. For a guest CPU computation, `VRAM_RESTORE` returns the committed 4,096 bytes in a caller-provided page buffer. The blocking stream protocol uses a 272-byte little-endian request, a signed 32-bit status, and a payload on success. Unknown operations, nonzero reserved bits and invalid offsets return `-EINVAL`. Publication or backing-store failures return `-EIO`. Serialize access to the stream across the entire request and reply.

### Multiple weight rows for one DIMM page

Use `vram_project_batch(port_fd, dimm_offset, rows, weights, columns)` with a contiguous `int32_t weights[rows][64]` input and `int64_t columns[rows][64]` output. Each output row is the projection for its corresponding weight row. Keep the page at its DIMM offset and exclude writes until the call returns. The return value is 1 if every tile used packed contraction, 0 for another successful route, or a negative error. Discard the output of a failed call, since earlier tiles may already have arrived.

A one-row call uses the existing scalar operation. For two or more rows, the broker accepts a 272-byte BATCH frame with the row count in the reserved field and the first weight row in the header. It validates the offset and flushes the DIMM once, then acknowledges with a 32-bit status. Only after a zero acknowledgement does the client send the rest of the first tile. Each subsequent tile carries up to 31 weight rows. For each tile the client waits for its status and columns before sending another. This bounds broker and FUSE buffers independently of the total row count. A 0.5.7 broker refuses the batch header before any trailing weights are sent, leaving its scalar port usable. A complete device error ends the request. After a broken transport, close the stream.
