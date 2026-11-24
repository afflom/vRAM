# vRAM

vRAM stores cold 4 KiB pages in the Verdant Engine 2.20.0 tensor runtime. Linux processes can use it as volatile swap. QEMU 9.2.0 can use it as guest main RAM. Both frontends link the same Rust library, use the same Prism-generated representation and scheduling decisions, and restore through the same page store. The Linux adapter speaks NBD over a local Unix socket. It persists no page data to disk.

On a write, the store measures candidate representations, including fills, sparse exceptions, packed fields, selectors, corrected selectors and tiled tensor programs. It admits tensor candidates after exact reconstruction. High-entropy pages can select the literal representation in the same store. On a read, the store reconstructs the original bytes. Linux and QEMU implement their respective fault and I/O interfaces. [Runtime source](device/src) · [Prism models and export evidence](device/proof) · [Verdant Engine](https://github.com/afflom/verdant-engine/releases)

## Use it on a Linux host

The host frontend has been exercised on x86-64 Linux 6.12 with 4 KiB pages, the NBD kernel module and swap support. Build prerequisites are Python 3.11+, a C toolchain and Rust. The recorded build used Rust 1.97.1. From a checkout run:

```sh
make -C host
sudo make -C host install
sudo modprobe nbd
```

Choose an unused whole NBD device. Start the server in a dedicated terminal or under a supervisor that keeps it alive for the entire swap session. This example advertises 128 MiB of logical swap. The server initializes a fresh swap header inside Verdant and locks its own memory to avoid recursive swapping. Linux zswap must be disabled while using this path.

```sh
sudo /usr/local/sbin/vram-host attach /dev/nbd0 128
```

In a second terminal, activate the device, inspect Linux accounting, and after use return its pages to the host before detaching:

```sh
sudo swapon /dev/nbd0
swapon --show
# After the workload has finished:
sudo swapoff /dev/nbd0 && sudo /usr/local/sbin/vram-host detach /dev/nbd0
```

For an exclusively Verdant experiment, configure this as the workload's only swap device. The installer leaves existing swap configuration to the operator. Allow sufficient host memory and locked-memory privileges for the server, page metadata, working set and literal pages. Killing the serving process while swap is active loses the device contents. Size the first deployment on a disposable host. A RAM-backed swap workload dominated by literal pages can exhaust the host.

Linux reports this capacity under `SwapTotal`. Physical `MemTotal` stays hardware-defined. QEMU reports the configured guest main RAM to the guest. The reachable working set depends on content, resident frames, metadata, search cost and the host's memory budget. This repository establishes neither a universal multiplier nor a host-independent maximum.

## Use it as guest main RAM

Build the QEMU backend using [the QEMU instructions](device/README.md). Select it as machine RAM:

```sh
-accel kvm -machine pc,memory-backend=ram,mem-merge=off -m 132 \
  -object memory-backend-verdant-tensor,id=ram,size=132M,x-quiescent-reclaim=on,x-resident-target=4M,x-search-nodes=49,x-adaptive-search=on
```

The verified configuration uses private 4 KiB memory, Linux userfaultfd and CPU-only guests with quiescent reclaim. Earlier receipts include TCG and KVM on an emulated AMD SVM host, with ordinary QEMU RAM as a control. [QEMU experiments](device/OPTIMIZER.md) · [Receipts](device/results)

## Automate optimization and scaling

The operator follows Verdant Studio's model-directed next-operation pattern. Three measured pilot trials use search-node budgets of 0, 7 and 49. They run through the same Verdant store as the two devices. The Prism model ranks byte-verified pilots by store charge, breaking ties by write time. It then decides whether each geometric capacity trial advances, stops at its measured RSS budget, finishes at its ceiling, or fails readback. The regional optimizer in the store also learns when to refit, skip and revisit searches. [Controller model](app/src/Contract.lex.tex) · [Studio operations](https://github.com/afflom/verdant-studio/tree/e6261b24e4c66631f3011c15be57b324f5f138fb/studio/src/Studio) · [Regional optimizer](device/src/search.rs)

Use Node 22+ and create a fresh receipt for each run:

```sh
npm ci --prefix automation --ignore-scripts
automation/node_modules/.bin/prism-code check .
automation/node_modules/.bin/prism-code build .
make -C host
node automation/operate.mjs F 256 96 measurements/local/factors.json
```

The arguments are workload, logical ceiling in MiB, process RSS budget in MiB and output file. Use `S` for rank-two pages, `F` for tiled factors, `M` for alternating structured and pseudorandom pages or `U` for pseudorandom bytes. The receipt records every pilot, selected budget, requested extent, actual page count, byte checks, store charge, RSS and timing. A partial final trial retains its actual verified count. RSS is sampled after groups of 64 writes and may cross the budget. Each trial runs in a fresh process.

For a matching workload, pass the receipt's `search_nodes` as the fourth argument to `vram-host attach`, or as QEMU's `x-search-nodes`. Calibration on representative pages matters. For the recorded 4 MiB tiled-factor pilot, the controller selected 49 nodes and reduced store charge from 4,210,808 to 710,184 bytes. The 64 MiB follow-up verified 16,384 pages and recorded 15,360 template refits. [Pilot and follow-up](measurements/scale-F.json)

The repository workflow builds the static Linux binary, runs calibration and uploads the binary, checksums and receipt. Its manual inputs select workload, ceiling and RSS budget. [Device workflow](.github/workflows/device.yml)

## Verification

The Linux integration test loads the distribution's NBD module in a disposable QEMU VM, activates only Verdant swap, writes ordinary anonymous memory, forces reclaim in a cgroup, and checks every byte after swap-in. The mixed 32 MiB case stored 4,096 modeled and 4,096 literal pages in one store. Linux registered 7,296 swap reads. The remainder could be served from its swap cache. [Linux receipt](measurements/linux-host.json)

To replay on a host with matching kernel and module files, build the static server and supply the Linux image, its XZ-compressed NBD module and QEMU's BIOS directory:

```sh
make -C host LDFLAGS=-static
python3 host/tests/boot.py --qemu /path/to/qemu-system-x86_64 \
  --kernel /path/to/vmlinuz --nbd /path/to/nbd.ko.xz --bios-dir /path/to/pc-bios \
  --result /tmp/vram-linux.json
```

The native rank-two trial wrote and read back 4,194,304 distinct page positions (16 GiB of logical contents). The store charged 637,534,328 bytes, and the process reached 657,208 KiB peak RSS. The pseudorandom trial selected literal pages and stopped during its 64 MiB attempt under a 48 MiB RSS budget. [Rank-two sweep](measurements/scale-S.json) · [Pseudorandom control](measurements/scale-U.json)

The native scale receipts measure the store in isolation. Linux swap receipts exercise the kernel path. The QEMU receipts measure the guest backend. [Native and Linux measurements](measurements) · [Proof scope and fault injection](docs/device-verification.md)
