# Using vRAM on Linux

## Virtualized Linux workstation

The `vram-machine` command uses stock QEMU, Linux FUSE and the Verdant page store. Install `qemu-system-x86` and `fuse3` alongside the package. Use an existing bootable VM image and the firmware settings it expects. The command runs in the foreground and owns its private runtime directory.

```sh
vram-machine run --state "$XDG_RUNTIME_DIR/workstation" --ram-mib 4096 --add-mib 4096 -- \
  -accel kvm -drive file=workstation.qcow2,format=qcow2
```

Run `vram-machine grow --state "$XDG_RUNTIME_DIR/workstation"` in a second terminal. Linux applications use the additional online System RAM through normal allocations. Check `lsmem` and `/proc/meminfo` inside the guest. The Linux boot option `memhp_default_state=online` selects automatic onlining. To have the entire capacity available from boot, set `--ram-mib 8192` and omit `--add-mib`.

The launcher reserves the configured total plus 4 KiB in an unlinked temporary file before boot. Unique dense literal bases use that file. Choose a local disk with sufficient free space using `--backing-dir DIRECTORY` or use the default user cache. Active pages, kernel cache and model metadata consume hypervisor RAM. Shut down the guest normally to release the temporary file and mount. The guest's VM disk holds persistent data. Like other volatile memory, this device loses its contents if the memory server or machine stops.

## Directly booted Linux swap

The package installs an amd64 server and a systemd service template. Services are enabled explicitly. Use a Linux kernel with NBD, swap and 4 KiB pages. Choose an unused whole NBD device. An occupied device fails attachment.

1. Check whether Linux zswap is active with `cat /sys/module/zswap/parameters/enabled`. vRAM can start when this reads N or the parameter is absent. If you choose to switch from zswap, use your distribution's boot configuration to set `zswap.enabled=0` and reboot. Existing ordinary swap devices can stay enabled.
2. Start vRAM with `sudo systemctl enable --now vram@nbd0.service`. This loads NBD, attaches the Verdant store and activates swap at priority 100. The service reports readiness after swapon succeeds.
3. Run your applications normally. Check `swapon --show` and `systemctl status vram@nbd0.service`. Run `sudo systemctl kill --kill-whom=main --signal=USR1 vram@nbd0.service` then `journalctl -u vram@nbd0.service -n 8` to see live store bytes, literal allocations, exact reuse, fast writes and dense residual captures.
4. To drain the device and disable automatic startup, use `sudo systemctl disable --now vram@nbd0.service`. If Linux needs more memory to complete swapoff, close applications or provide another swap device. The service keeps serving pages while it retries.

The default logical size is one eighth of Linux MemAvailable at startup, rounded down to MiB and capped at 1 TiB. Only touched groups allocate page metadata. Edit `/etc/default/vram` to set `VRAM_SIZE_MIB` explicitly or change `VRAM_SEARCH_NODES` (0–49). Apply settings with an orderly service restart. Allow room for literal payloads, the applications' resident pages, allocator overhead and the kernel. RAM-only swap shares physical capacity with applications.

Keep the serving process alive while its swap is active. Power loss, SIGKILL or a server crash can lose paged-out data. Use a disk-backed swap device for hibernation. The service never writes page payloads to disk itself.

Source, verification receipts and QEMU guest RAM instructions are at https://github.com/afflom/vRAM.
