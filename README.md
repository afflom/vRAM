# vRAM

[![vRAM product preview](site/assets/social-preview.png)](https://afflom.github.io/vRAM/)

[Product and downloads](https://afflom.github.io/vRAM/) · [Prism facets](https://afflom.github.io/vRAM/facets.html) · [Inspect the executable model](https://afflom.github.io/vRAM/runtime/#facet-heading) · [Brand guide](docs/brand.md) · [Publishing and verification](docs/product-presentation.md)

vRAM supplies System RAM to a virtualized Linux operating system. The packaged `vram-machine` launcher boots an existing VM disk on Verdant-backed memory with stock QEMU and can plug an additional DIMM while applications run. Linux sees the memory in `/proc/meminfo` under `MemTotal`. The separate `vram-host` service supplies volatile swap to a directly booted Linux system. Both use the same Verdant Engine 2.20.0 store and Prism representation policy.

On a write, the store measures candidate representations, including fills, sparse exceptions, packed fields, selectors, corrected selectors and tiled tensor programs. It admits tensor candidates after exact reconstruction. High-entropy pages can select the literal representation in the same store. On a read, the store reconstructs the original bytes. Linux and QEMU implement their respective fault and I/O interfaces. [Runtime source](device/src) · [Prism models and export evidence](device/proof) · [Verdant Engine](https://github.com/afflom/verdant-engine/releases)

## Install on a Linux computer

Download the Ubuntu 24.04 amd64 package and its checksum. Run the following commands from the download directory. The local build below uses your distribution's libfuse3 ABI. [Linux package](https://afflom.github.io/vRAM/downloads/vram-host_0.6.1_amd64.deb) · [SHA256SUMS](https://afflom.github.io/vRAM/downloads/SHA256SUMS)

```sh
sha256sum -c SHA256SUMS
sudo apt install ./vram-host_0.6.1_amd64.deb qemu-system-x86 fuse3
```

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

Version 0.5.9 coalesces DIMM range reads across consecutive backing slots. Ordinary guest memory reads use this automatically. A 1 GiB sequential readback of independent pages took 4,096 backing reads using 64-page requests. [Measurements and verification](docs/device-verification.md#range-reads-across-consecutive-file-slots)

Version 0.5.8 adds a batched projection call. A guest can send multiple signed weight rows for one stable DIMM page, pay for publication once, and receive the results in row order. [Batch API](host/packaging/USAGE.md#multiple-weight-rows-for-one-dimm-page) · [Scaling measurements](docs/device-verification.md#streamed-projection-batches)

In 0.5.7 a trusted guest can request a signed matrix projection on a page of its online vRAM DIMM. Start the VM with `--projection` and `--add-mib`. The packed route returns 64 sums directly from the Verdant tensor core. [Guest interface](host/packaging/USAGE.md#guest-projection-port) · [Paired benchmark](docs/device-verification.md#guest-projection-on-an-online-dimm)

In 0.5.6, range reads restore adjacent pages sharing a dense base with one file read per run inside a policy group. Range writes reuse the last immutable file candidate during exact admission. [Read and write evidence](docs/device-verification.md#shared-literal-io-in-056)

In 0.5.5, concurrent writes can reuse a prepared page encoding across regional policy changes. A compatible cached template advances the current group's credits and preserves its next full search. [Validation and overlap measurements](docs/device-verification.md#revalidating-prepared-work-in-055)

In 0.5.4, contended full-page writes can fit a Verdant representation while other requests use the store. Commit rechecks the selector, regional search state and live dictionary. [Concurrency validation](docs/device-verification.md#parallel-representation-fitting)

In 0.5.3, the memory server batches full-page reads and writes and applies partial-page updates atomically under concurrent requests. [Concurrency tests and profiling](docs/device-verification.md#concurrent-memory-file-requests)

In 0.5.2, overwriting a shared dense page can reuse its immutable base even when an edit changes the lookup fingerprint. Converging overwrites also shrink the dictionary as bases retire. [Readback, I/O counts and benchmark methodology](docs/device-verification.md#ownership-aware-admission-in-052)

In 0.5.1, returning a file-backed dense slot uses metadata already allocated on admission. Interrupted writes retain the old logical page. [Fragmented reclaim, partial-write tests and timings](docs/device-verification.md#dense-slot-reclamation-in-051)

In 0.5.0, a 1 PiB logical store passed sparse writes, exact reads and range discards under a 64 MiB process limit. The fully populated independent-data test covered 1 GiB with a disk reserve. [Capacity proofs, scaling measurements and paired timings](docs/device-verification.md#configured-capacity-and-sparse-scaling-in-050)

### Run your operating system on vRAM

Use an existing bootable VM disk. This example gives the operating system 8 GiB at boot. Supply the drive, firmware and other QEMU options you normally use for this image after `--`. The launcher owns the memory configuration and private management socket. Run it as your normal QEMU user.

```sh
vram-machine run --state "$XDG_RUNTIME_DIR/workstation" --ram-mib 8192 -- \
  -accel kvm -drive file=workstation.qcow2,format=qcow2
```

For live growth, reserve a DIMM when starting the machine. This example boots with 4 GiB and adds another 4 GiB from a second terminal while applications keep running.

```sh
vram-machine run --state "$XDG_RUNTIME_DIR/workstation" --ram-mib 4096 --add-mib 4096 -- \
  -accel kvm -drive file=workstation.qcow2,format=qcow2
```

```sh
vram-machine grow --state "$XDG_RUNTIME_DIR/workstation"
```

In the virtualized OS, use `lsmem` and `grep MemTotal /proc/meminfo`. Linux onlines the DIMM according to its memory-hotplug policy. For guests configured to leave new memory offline, use the Linux boot option `memhp_default_state=online`. Repeating `grow` reports the same DIMM. Shut down through the guest normally.

Both memory ranges map into one private Verdant store. Linux manages the shared mappings and their writeback. Verdant can represent a page as a tensor program, reuse a dense base, store its changed coordinates, or keep its exact literal bytes. Unique literal bases use the pooled private reserve. The launcher reserves the configured boot plus expansion capacity and one 4 KiB replacement slot before starting QEMU. The default filesystem is the user's cache directory. Supply `--backing-dir /path/to/local-ssd` to choose another. The operating system needs working DRAM for hot pages, page tables and the store's metadata. Disk reads and writes have storage latency. Guest shutdown releases the reserve.

A native store trial wrote and restored 1 GiB of unique pseudorandom pages with a 64 MiB process address-space limit. All 262,144 pages used their own literal slots in the reserved file. Process RSS excludes filesystem page cache. [Independent-page measurement](measurements/file-backed-independent.json) · [Linux hot-add measurement](measurements/linux-virtual-dimm.json)

In 0.4.1, sparse edits to dense pages reuse the same base read for fitting and exact reconstruction. Median write time on the 128 MiB edited workload fell from 176.7 to 70.2 ms across six paired runs. The deliberately colliding four-base control rose from 43.2 to 45.4 ms. A failed literal write preserves the previous page and releases any newly allocated empty group. [Workloads, read counts and failure tests](docs/device-verification.md#file-backed-admission-and-write-rollback-in-041)

### Add swap to a directly booted Linux system

The package installs a service and configuration. If you upgraded a running instance, use `sudo systemctl restart vram@nbd0.service` to drain its pages and load the updated binary. Choose an unused whole NBD device on a Linux kernel with 4 KiB pages and swap support. Check `/sys/module/zswap/parameters/enabled` first. If it reads Y, choose whether to switch off zswap using your distribution's `zswap.enabled=0` boot option. vRAM leaves the system-wide setting and existing swap devices under your control.

```sh
sudo systemctl enable --now vram@nbd0.service
swapon --show
```

Applications can now page to Verdant through the normal Linux memory manager. The service starts at boot, reports readiness after swap activation, and drains the device before disconnecting. Its default logical size is one eighth of Linux `MemAvailable` at startup. The page directory grows on first write and releases empty groups on discard. To change the logical size or search budget, edit `/etc/default/vram` and restart the service. To remove it, first run `sudo systemctl disable --now vram@nbd0.service`, then `sudo apt remove vram-host`. [Service configuration, statistics and shutdown](host/packaging/USAGE.md)

Linux accounts for this capacity under `SwapTotal`. Both literal and modeled pages consume host RAM. Capacity gains depend on the data and the resident working set. A process crash or loss of power can lose paged-out data. The service needs memory and locked-memory privileges for the store and headroom for Linux to drain it.

On a Debian 13 amd64 VM, the installed service backed an in-memory SQLite database running under a 40 MiB cgroup limit. SQLite verified all 20,480 rows (80 MiB of payloads) after updates and passed its integrity check. The test requires Linux to swap pages out and back in during the workload. The service drained on stop and automatically reactivated after reboot. [Installed-package test](measurements/debian-installed.json)

To build the package locally, install a C compiler, Python 3.11+, Rust, `pkg-config`, `libfuse3-dev` and `dpkg-dev`, then run `make -C host LDFLAGS=-static` and `python3 host/package.py vram-host_0.6.1_amd64.deb`. Other systemd-based Linux distributions can build the binary with `make -C host`, run `sudo make -C host install-service` and then `sudo systemctl daemon-reload` before enabling the instance. The standalone server also supports `vram-host run /dev/nbd0 auto` on compatible Linux systems. The original `attach` and `detach` commands remain available for callers that manage swapon and swapoff themselves. With `attach`, wait for `blockdev --getsize64 /dev/nbd0` to report the requested byte capacity before calling `swapon`.

## How dense pages interact

Two pages with high byte diversity can share every coordinate, or differ at only a few. Verdant indexes literal bases across the running store. Prism grows and shrinks its bucket directory with the number of live bases. Unrelated fingerprints that land in one bucket remain searchable. Up to four candidates with identical fingerprint samples receive full byte comparisons. A fingerprint proposes a candidate. Verdant compares the bytes before sharing an immutable allocation. For near matches it builds a sparse list of coordinate-wise XOR differences and admits it after exact reconstruction. A write to the original swap address cannot change any dependent page. Discarding or overwriting the final page that references a literal also frees the lookup table.

Independent pages can still inform scheduling. In a 64-page region, a full literal result allows the Prism scheduler to reduce subsequent fitting work when samples on both axes have high byte diversity. The credits expire and trigger another full probe. This saves CPU on sustained high-entropy writes. Sampling can pass over a compressible page until a later probe. Every representation retains the original bytes.

The native comparison times 128 MiB of writes with the same harness and input generator against the pre-sharing library (commit `0d24329`) and vRAM 0.2.0. R cycles through 256 pseudorandom bases. D makes four pseudorandom byte edits to each recurrence. U generates each page independently. The measurements include full readback and three trials in alternating order. [Native comparison](measurements/literal-reuse.json) · [Managed repeated pages](measurements/managed-repeated.json) · [Managed dense residuals](measurements/managed-dense.json) · [Independent control](measurements/managed-independent.json)

| 128 MiB written and read back | Pre-sharing store | 0.2.0 store | Median write time | Median readback time |
|---|---:|---:|---:|---:|
| Repeated dense literals | 128.51 MiB | 1.78 MiB | 0.493 s → 0.021 s | 0.025 s → 0.020 s |
| Dense literals with four edits per recurrence | 128.51 MiB | 6.02 MiB | 0.279 s → 0.124 s | 0.027 s → 0.021 s |
| Independent pseudorandom pages | 128.51 MiB | 129.77 MiB | 0.286 s → 0.117 s | 0.026 s → 0.031 s |

Store charges include the directory, lookup table and live representations. Process RSS, kernel memory and allocator overhead can be higher. The independent control spent more bytes on metadata and less time searching for representations.

## Scaling a dense working set

The wide-dictionary workload cycles through 32,768 distinct pseudorandom 4 KiB pages (128 MiB of unique data). With 512 MiB written, the median store charge across three native trials fell from 432.77 MiB in 0.2.1 to 132.39 MiB in 0.3.0. The newer store kept exactly 32,768 bases. Under a 768 MiB RSS budget, the earlier build stopped after verifying 928.75 MiB of a 64 GiB request. The newer build verified all 64 GiB with 403.63 MiB peak RSS.

The independent 256 MiB control used 259.30 MiB of store in 0.2.1 and 261.63 MiB in 0.3.0. Median write times in that run were 0.294 and 0.322 seconds respectively. A separate six-phase mixed trace exercised writes, overwrites, collisions and discards across 65,536 addresses. Both versions checked every byte after each phase. The new store returned to its 208-byte initial charge on the final discard. Store charge measures owned data structures. Process RSS also includes allocations retained by the system allocator. [Paired trials and mixed trace](measurements/wide-dictionary.json)

The Prism-controlled sweep selected a search budget of seven nodes and advanced through a 256 GiB logical trial. It verified all 67,108,864 pages with 1.19 GiB peak process RSS. Each logical page still needs a directory entry. The measured working set contained 128 MiB of distinct payloads. [Controller checkpoints](measurements/scale-wide-256gib.json)

In a separate Linux trial, a 1 GiB VM used the host server's NBD swap path for a 4 GiB anonymous mapping. Before readback, 1,048,576 page-table entries were swapped and 0 were present. Linux registered 1,047,347 swap-ins. The application checked every byte, and the service drained cleanly. [Linux working-set trial](measurements/linux-wide-4gib.json)

## Use the custom userfaultfd QEMU backend

Build the QEMU backend using [the QEMU instructions](device/README.md). Select it as machine RAM:

```sh
-accel kvm -machine pc,memory-backend=ram,mem-merge=off -m 132 \
  -object memory-backend-verdant-tensor,id=ram,size=132M,x-quiescent-reclaim=on,x-resident-target=4M,x-search-nodes=49,x-adaptive-search=on
```

The verified configuration uses private 4 KiB memory, Linux userfaultfd and CPU-only guests with quiescent reclaim. The rebuilt frontend passed nested TCG and KVM runs against the digits corpus and a Linux image. KVM ran on a TCG-emulated AMD SVM host. Earlier capacity trials include ordinary QEMU RAM as a control. [TCG replay](measurements/qemu-tcg.json) · [KVM replay](measurements/qemu-kvm.json) [QEMU experiments](device/OPTIMIZER.md) · [Receipts](device/results)

## Automate optimization and scaling

The operator follows Verdant Studio's model-directed next-operation pattern. The model supplies the pilot budgets (0, 7 and 49), returns each next extent, clamps it to the ceiling, and assesses raw page counts and RSS observations. They run through the same Verdant store as the two devices. The Prism model ranks byte-verified pilots by store charge, breaking ties by write time. It decides whether each geometric capacity trial advances, stops at its measured RSS budget, finishes at its ceiling, or fails readback. The regional optimizer in the store also learns when to refit, skip and revisit searches. [Controller model](app/src/Contract.lex.tex) · [Studio operations](https://github.com/afflom/verdant-studio/tree/e6261b24e4c66631f3011c15be57b324f5f138fb/studio/src/Studio) · [Regional optimizer](device/src/search.rs)

Use Node 22+ and create a fresh receipt for each run:

```sh
npm ci --prefix automation --ignore-scripts
automation/node_modules/.bin/prism-code check .
automation/node_modules/.bin/prism-code build .
make -C host
node automation/operate.mjs F 256 96 measurements/local/factors.json
```

The arguments are workload, logical ceiling in MiB, process RSS budget in MiB and output file. Use `S` for rank-two pages, `F` for tiled factors, `M` for alternating structured and pseudorandom pages, `U` for independent pseudorandom bytes, `R` for repeated literals `D` for dense pages with sparse edits or `W` for 32,768 recurring dense bases. The controller accepts logical ceilings up to 1 TiB and checks the RSS budget at each trial. The receipt records every pilot, selected budget, requested extent, actual page count, byte checks, store charge, RSS and timing. A partial final trial retains its actual verified count. RSS is sampled after groups of 64 writes and may cross the budget. Each trial runs in a fresh process.

For a matching workload, pass the receipt's `search_nodes` as the fourth argument to `vram-host attach`, or as QEMU's `x-search-nodes`. Calibration on representative pages matters. For the earlier 4 MiB tiled-factor pilot, the controller selected 49 nodes and reduced store charge from 4,210,808 to 710,184 bytes. The 64 MiB follow-up verified 16,384 pages and recorded 15,360 template refits. [Pilot and follow-up](measurements/scale-F.json)

The repository workflow builds the static Linux binary, runs calibration and uploads the binary, checksums and receipt. Its manual inputs select workload, ceiling and RSS budget. [Device workflow](.github/workflows/device.yml)

## Verification

The device workflow reports the highest measured guest write throughput at each attainable p99 budget for the tested VM widths. It replays all four runs per width through the Prism performance model. [Timing assumptions, proofs and reproduction](docs/device-verification.md#latency-and-throughput-bounds)

The Linux integration test loads the distribution's NBD module in a disposable QEMU VM, activates only Verdant swap, writes ordinary anonymous memory, forces reclaim in a cgroup, and checks every byte after swap-in. The mixed 32 MiB case stored 4,096 modeled and 4,096 literal pages in one store. Linux registered 7,168 swap reads. The remainder could be served from its swap cache. [Linux receipt](measurements/linux-host.json)

In a separate test, a 512 MiB Linux VM ran a 2 GiB anonymous-memory workload in a 48 MiB cgroup, with the Verdant server outside that cgroup. The program wrote and checked every byte. Linux counted 523,170 swap reads. The store held 524,288 rank-two pages and charged 80,166,015 bytes including its directory. Swapoff and detach both completed. This test uses generated structured data. [2 GiB Linux receipt](measurements/linux-host-2gib.json)

To replay on a host with matching kernel and module files, build the static server and supply the Linux image, its XZ-compressed NBD module and QEMU's BIOS directory:

```sh
make -C host LDFLAGS=-static
python3 host/tests/boot.py --qemu /path/to/qemu-system-x86_64 \
  --kernel /path/to/vmlinuz --nbd /path/to/nbd.ko.xz --bios-dir /path/to/pc-bios \
  --result /tmp/vram-linux.json
```

Add `--mib 2048 --profile S` to replay the larger Linux trial.

The earlier native rank-two trial wrote and read back 4,194,304 distinct page positions (16 GiB of logical contents). The store charged 637,534,328 bytes, and the process reached 657,336 KiB peak RSS. The pseudorandom trial selected literal pages and stopped during its 64 MiB attempt under a 48 MiB RSS budget. [Rank-two sweep](measurements/scale-S.json) · [Pseudorandom control](measurements/scale-U.json)

The native scale receipts measure the store in isolation. Linux swap receipts exercise the kernel path. The QEMU receipts measure the guest backend. [Native and Linux measurements](measurements) · [Proof scope and fault injection](docs/device-verification.md)
