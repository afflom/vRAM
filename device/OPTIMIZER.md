# Programming the vRAM optimizer

vRAM optimizes the charged bytes of a cold 4 KiB guest page subject to exact reconstruction and a bounded amount of search. A learned plan describes how to encode bytes. Every admitted page owns its dictionary, indices, literal bytes and corrections. An ordinary guest store can replace any of them.

## The optimization loop

1. Build an incumbent from uniform, sparse, palette, shared binary-selector and raw representations. An existing binary selector can retain a short list of exact byte corrections. The verified chooser compares the sizes supplied by the encoder. Pages already costing at most 256 bytes bypass the additional search.
2. On an eligible page, treat the bytes as a 64 × 64 matrix. The search can cut a rectangle horizontally or vertically, store it literally, fill it, or factor it along either axis. Equal row or column vectors form classes. Their number and contents determine a dictionary and a packed one-hot index. A leaf can have 1–16 classes. Cuts stop at 16 cells per side.
3. Solve overlapping tile subproblems once per search. The full grid has 49 distinct dyadic rectangles. The planner compares actual serialized lengths, including tags, ranks, packed labels, dictionary data, literal tails and both children of a cut. Raw leaves allow a profitable part of a page to coexist with an incompressible part. A configurable limit bounds the number of evaluated tiles per eligible capture. The limit counts both a failed shape refit and any subsequent search.
4. Reconstruct the proposed page twice. Each factor leaf is checked with Verdant's released integer packed contraction, and the normal fault decoder expands its one-hot lanes with a LexLean-generated word kernel. The native encoder compares both pages with the guest's original bytes. Publication also requires a strict decrease in per-page backing charge. A failed check preserves the incumbent.
5. Cache only the winning operator tree for each region of 64 guest pages. The next eligible capture can refit that tree with its own ranks, labels and values. A failed refit spends the remaining evaluation budget on a new search. A failed full probe schedules a bounded interval of skipped searches. After 15 subsequent eligible captures in the region, the next eligible capture probes again. Equal unchanged modeled bytes can retain the current backing without advancing the scheduler.

The local model roots control eligibility, split limits, factor bounds, strict replacement, the choice to probe/refit/skip, and expiration of the regional credits. Rust enumerates candidates, keeps the bounded work counter and handles storage. The search minimizes the backing charge of the candidates it considers. Search CPU, stall time and expanded frames are measured separately. A skipped probe can miss a profitable one-off page. The cost model currently gives priority to cold capacity.

## RAM path and scope

A TCG read fault can install a clean write-protected page in one userfaultfd copy. The first write marks it dirty. A clean eviction reuses the committed value. KVM captures every evicted page conservatively because vCPUs can cache writable secondary mappings. The fallback on a TCG host without write tracking also captures every evicted page. Linux invalidates mappings on discard. Either accelerator pauses its vCPUs before automatic reclaim. The operator supplies an exclusive CPU-only configuration, without DMA, long-lived pins or asynchronous host writes into managed frames.

The automatic governor samples the resident count every 10 milliseconds. It selects frames from nonempty groups of 64, using an indexed list and word-sized membership sets. Selection work scales with the number of frames evicted. The configured target is soft and excludes the store, metadata, QEMU, and kernel memory. The telemetry separates the process cgroup charge from estimated backing allocations. The counters include global backend pages such as guest stack pages.

The shipped device is a QEMU main-memory backend. Resident loads and stores use ordinary mapped frames. Its tested scope includes runtime capture, eviction, refault and readback. A general host memory hotplug driver, unrestricted DMA/VFIO, live migration and crash recovery would require further implementation. Hardware KVM performance requires a physical host with virtualization exposed. The KVM functional trials here use Linux KVM with AMD SVM emulated by the outer QEMU.

## Measured search cost

The native benchmark uses 128 distinct 4 KiB pages per run. Capture includes model inference, exact acceptance and allocation. Each restore scan reads every page and independently checks its bytes. Times below are median process CPU microseconds per page across two six-run series; verification is outside the timed restore interval. Backing bytes include the directory and regional feedback storage. Allocation rounding, scratch space and resident frames are excluded from this native count.

| Input | Node cap | Policy | Backing bytes | Full probes | Refits | Tile evaluations | Capture µs | Restore µs |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| F | 0 | fixed | 526,456 | 0 | 0 | 0 | 6.97 | 0.25 |
| F | 1 | fixed | 526,456 | 128 | 0 | 128 | 8.04 | 0.25 |
| F | 7 | fixed | 291,448 | 128 | 0 | 896 | 43.54 | 1.24 |
| F | 49 | fixed | 88,824 | 128 | 0 | 6,272 | 124.33 | 2.58 |
| F | 49 | adaptive | 88,878 | 8 | 120 | 752 | 81.55 | 2.74 |
| S | 0 | fixed | 19,576 | 0 | 0 | 0 | 56.43 | 0.42 |
| S | 49 | fixed | 19,576 | 0 | 0 | 0 | 47.11 | 0.38 |
| S | 49 | adaptive | 19,624 | 0 | 0 | 0 | 41.83 | 0.32 |
| U | 0 | fixed | 526,456 | 0 | 0 | 0 | 6.97 | 0.26 |
| U | 49 | fixed | 526,456 | 128 | 0 | 6,272 | 43.34 | 0.31 |
| U | 49 | adaptive | 526,504 | 8 | 0 | 392 | 10.66 | 0.25 |

The one-node search cannot split the two-axis workload. Seven evaluations find a partial solution. The full search finds both factors, then the adaptive policy can reuse their layout. The conventional binary-selector workload already fits in 136 bytes of page payload and bypasses the new search. The pseudorandom workload makes the unsuccessful-search cost visible. The archive includes every observation in `results/adaptive-cpu.json` and `results/adaptive-cpu-repeat.json`. The second series ran after the other short VM trials finished.

In a separate selector-only native trial, the previous QEMU backend's store took 33.72 µs per 4 KiB restore and the generated word kernel took 0.22 µs. Each observation timed 16,384 restores and verified all bytes. This is page-store CPU time and excludes page faults, guest execution and QEMU scheduling.

## Measured guest capacity

Both arms advertise the same memory and run identical guest code. The hot window is 1 MiB. For vRAM the resident frame target is 4 MiB. Guest reads and writes never tell the pager which addresses to evict. At geometric checkpoints the guest regenerates and verifies every byte previously written. A fresh cgroup v2 limits each inner QEMU with swap disabled. Peak charge includes the process, its backing and kernel charges; shared binaries or file pages can be charged elsewhere. The largest verified checkpoint can precede the allocation which triggered an OOM kill. A completed test ceiling is a lower bound for that workload.

| Guest experiment | Cgroup limit, MiB | Ordinary verified, MiB | vRAM verified, MiB | vRAM peak charge, MiB |
| --- | ---: | ---: | ---: | ---: |
| KVM, split-axis pattern, search off | 32 | 16 | 16 | 32.00 |
| KVM, split-axis pattern, adaptive search | 32 | 16 | 32 | 21.41 |
| TCG, pseudorandom | 32 | 16 | 16 | 32.00 |
| TCG, binary-selector pattern | 80 | 32 | 1024 | 57.01 |

At the final synthesized KVM checkpoint the store reported 8,193 program pages across the backend, 514 full probes and 7,699 successful refits. The two KVM experiments used the same stripped executable and same guest ROM. In the GiB trial, 262,144 pages used the original binary-selector model. Its verified allocation was 1,024 MiB and its final full scan took 118.2 emulated-host seconds. The GiB result measures a different, deliberately model-friendly distribution. It uses a private no-reserve mapping in both arms.

A stock Debian Linux guest separately edited one byte on each of 241 pages in a 16 MiB modeled range. It verified every byte after reclaim. All 4,096 backings retained a selector, 241 byte corrections were stored, and the following read-only pass produced 4,096 clean frame drops and 0 fresh captures under TCG.

The mixed 8 MiB corpus included 115,008 digit intensities and a 4 MiB compressed Linux kernel prefix. It passed byte-by-byte restoration, cross-page writes, integer projections, whole-page pseudorandom replacement and combined manual/automatic evictions. The initialized corpus admitted one synthesized page at 4,093 charged bytes versus 4,096 raw bytes. All 2,048 pages of the independent pseudorandom overwrite selected raw backing. The Linux KVM image-seeded inner guest also verified its hashes, writes and initially zero memory.

## Reproduce

The source archive contains the input models, generated packages, optimizer, QEMU adapter, firmware source and raw measurements. The build validates the exact receipts and linked generated package files. The three production bundles use LexLean 0.4.0 with Lean 4.32.1. The two supplemental Lean runs rebuild their imports and match their compiled module hashes to the production attestations. These proofs cover the exported decisions and the stated algebraic laws. The Rust search, allocator, QEMU, Linux and hardware are outside the checked proofs.

```sh
python3 build.py --verify-only
for bundle in proof/bundle proof/runtime/bundle proof/search/bundle; do
    lexlean production verify-artifact "$bundle"
done
python3 proof/runtime/check_laws.py
python3 proof/runtime/check_laws.py --search
python3 build.py
python3 install.py ../qemu-9.2.0
python3 validation/optimizer.py --library target/release/libverdant_memory.a --result native.json
```

After building QEMU as described in README.md, the following command runs the paired firmware capacity test directly on a Linux KVM host. Supply a writable, delegated cgroup v2 parent with the memory controller enabled for its children. Each arm gets its own child with swap disabled. The runner reads each limit and records the kernel's OOM counters. The firmware verifies every guest byte at checkpoints.

```sh
python3 validation/host_capacity.py --qemu ../qemu-build/qemu-system-x86_64 \
  --bios-dir ../qemu-9.2.0/pc-bios --cgroup-parent /path/to/delegated/group \
  --accel kvm --profile F --maximum-mib 32 --budget-mib 32 --resident-mib 4 --result kvm.json
```

To reproduce the self-contained KVM experiment, use the outer Linux image and its matching AMD SVM modules. Omit the module option to run an inner TCG VM. Run the same command with `--search-nodes 0` to disable synthesis, or with `--fixed-search` to probe each eligible page.

```sh
python3 validation/reclaim.py --qemu ../qemu-build/qemu-system-x86_64 \
  --kernel /path/to/vmlinuz --bios-dir ../qemu-9.2.0/pc-bios \
  --svm-modules /path/to/matching/lib/modules/kernel-version \
  --capacity-profile F --maximum-mib 32 --budget-mib 32 --resident-mib 4 --geometric --result nested.json
```

`F` generates a changing five-class column pattern in the upper half of a page and a seven-class row pattern in the lower half. `S` generates the original shared binary selector. `M` alternates that pattern with pseudorandom pages. `U` generates independent pseudorandom page contents. The governor runs without guest range hints whenever a nonzero resident target is set.

[Verdant 2.20.0](https://github.com/afflom/verdant-engine/releases/tag/v2.20.0) · [LexLean 0.4.0](https://github.com/UOR-Foundation/LexLean/releases/tag/v0.4.0) · [QEMU 9.2.0 source](https://download.qemu.org/qemu-9.2.0.tar.xz)
