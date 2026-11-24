# vRAM verification

The root Prism project defines capacity decisions and pilot selection. Build and verify the checkout with Prism Code 0.11.0. The model carries 28 theorems and 33 executable acceptance vectors. The project verifier checks the built holo against the model on disk and replays its vectors. The artifact reports evidence level P0. Its project verification covers identity and replay. It carries no separately signed release attestation.

The runtime uses the published Verdant tensor crate and twelve local LexLean exports packaged in the locked offline Cargo registry. `python3 device/build.py --verify-only` verifies the six export receipt digests and their attestations, kernel-checked status, every registered binding, and the archive contents actually linked by Cargo. The host transport passes writes, reads and discards to this runtime. NBD negotiation, Linux reclaim, memory locking, the allocator, page I/O and the measurement harness are systems code exercised by integration tests. The proofs cover their named model definitions.

## Fault injection

Changing the failed-readback branch of `decision` from halt to advance made `prism-code check .` exit 1. It reported `contract.theorem-unproved` for VR-02 and `proof.rejected` for `failedBytesStop`. Restoring the model passed the project check, build and vector replay.

Appending one byte to the locked `vram_word` archive in a temporary checkout made `python3 device/build.py --verify-only` exit 1 with `locked archive differs: vram_word`. The unaltered archive passed. A Linux test that wrote and immediately reread anonymous pages produced 8,192 swap writes but zero Verdant restores. The integration gate rejected it. The current test applies cgroup pressure, checks actual swap reads and selector restores, and compares every byte.

The multi-GiB fixture budgets for page tables when it forces reclaim. It releases the byte-verified mapping before invoking the detach command. With the 2 GiB mapping still present and swap already disabled, Linux refused that fork with `ENOMEM`. The final fixture requires successful readback, swapoff, server exit and detach before writing a passing receipt.

## Managed host and cross-page reuse

The canonical host model is `app/src/HostPolicy.lex.tex`. The native Rust crates for autosizing and empty-group release came from a kernel-checked production build of that same file. The build verifies the source against the production input manifest. To reproduce with the pinned LexLean compiler, copy `device/proof/host/{lexlean.toml,lexlean.lock,lean-toolchain,lakefile.toml,lake-manifest.json,production.json}` to an empty project and the canonical source to its `src/`, then run `lexlean check` and `lexlean production build --request production.json`.

A planted rule that released every group failed the Prism theorem `HostPolicy.retainOccupied`. The modified source also failed the offline runtime verifier with `host policy differs from the certified native export`. Suppressing XOR application in the dense residual decoder failed the collision, overwrite and discard regression. The restored gates pass. A sparse directory regression exercises the first and final pages of a 1 TiB logical store, partial discard across a group boundary and release of the last occupied leaf. It checks that empty metadata costs under 256 KiB and returns to its original size after discard.

The Debian package installs disabled. The managed Linux fixtures require a readiness datagram after real swapon, positive kernel swap traffic, all bytes restored, live engine counters and termination through successful swapoff. A live 1 MiB anonymous range is checked again after service exit. The R, D and U fixtures exercise exact sharing, dense residuals and independent literal writes respectively. Reproduce with `host/tests/boot.py --managed --profile D` and the kernel, NBD module, QEMU and output arguments in the README.

The regression `final_literal_releases_index_on_overwrite_and_trim` exercises the last owner of a dense base. On the previous implementation, `cargo test --locked --offline final_literal_releases_index_on_overwrite_and_trim` in `device/` reported 265,496 bytes after a residual was overwritten with a fill. The same group with only a fill required 3,320 bytes. The fixed test recovers that baseline, reuses the index on subsequent writes, then returns to its empty-store charge after trim. The lifetime reuse counters survive both releases.

The distribution fixture checks that a fresh package installs disabled, enables the instance, exercises SQLite under kernel reclaim and verifies the next boot. Its observer orders itself after the service and has no dependency that starts it. Disabling the service after the first successful drain in a temporary copy produced `VRAM_REBOOT_FAILED` on the second boot and exit status 1. The enabled instance passed on the same Debian 13 amd64 image. With QEMU and genisoimage installed, run `python3 host/tests/distribution.py --image debian-13-generic-amd64.qcow2 --deb vram-host_0.3.0_amd64.deb --result debian-installed.json`. QEMU uses a temporary writable overlay. The receipt identifies the image and package by SHA-256. [Debian receipt](../measurements/debian-installed.json)

To repeat the native comparison, build the previous library from commit `0d24329` and the current one, then run `python3 device/validation/literal_reuse.py --previous-library /path/to/libverdant_memory.a --previous-commit 0d24329 --result comparison.json`. Times include input generation on the same machine and should be compared within a run.

## Dictionary resizing and workload changes

The production export of `app/src/IndexPolicy.lex.tex` takes bucket count and live-base count and chooses initialization, growth, shrink, release or hold. Reproduce it as for HostPolicy using the corresponding inputs from `device/proof/index/`. Both models are bound to their root project sources during the offline build. Changing the growth comparison from inclusive to strict in a temporary checkout made `prism-code check` reject `IndexPolicy.growAtLoad` for VR-10. The restored model passes its boundary vectors. The capacity controller also replays the existing five-character extent format, larger six- and seven-character responses, and rejection above 1 TiB.

The Rust regression grows the dictionary past the old fixed limit, forces a capacity reservation error, and looks up the surviving bases after partial trim. A failed optional index rebuild preserves the previous table and the page owners. Another regression reads whole pages while a writer overwrites and trims the same addresses. The Linux fixture reads every entry in the application mapping through `/proc/self/pagemap` before readback. It requires at least seven eighths of the mappings to be swapped, checks physical eviction with mincore, requires kernel swap writes covering the evicted pages and swap-ins covering at least three quarters of the mapping, then checks every application byte. This accounts for pages Linux keeps resident or in its swap cache. The Debian package fixture stages and hashes the same bytes used to build its boot media. Replacing the input path with the 0.2.1 package after media creation left the verified 0.3.0 digest intact.

To run the matched comparison, supply the release library from commit `329452a` to `python3 device/validation/wide_dictionary.py --previous-library /path/to/libverdant_memory.a --previous-commit 329452a --result wide.json`. Both libraries receive the same C generator, extent, search setting and RSS budget. The comparison records actual verified counts even on budget stops. The mixed trace includes structured pages, independent literals, sparse edits, coincident fingerprint samples and a full drain. Each C process exits on the first byte mismatch.

## Virtualized System RAM and disk-backed literals

VR-11 and VR-12 are measured systems capabilities. The packaged launcher maps two disjoint extents of one Verdant store through stock QEMU's shared file memory backend. The inner Linux workload compares `MemTotal` on either side of a live ACPI DIMM addition, keeps swap disabled, writes ordinary anonymous memory and checks every byte. It reads its own pagemap to identify physical placement. The test manager repeats the grow request, synchronizes both memory files and reads an added physical page with `O_DIRECT`. That page must match the guest's independently generated bytes after FUSE writeback. It requires a clean guest shutdown and removal of the runtime mount. Run on a Linux machine with FUSE:

```sh
python3 host/tests/dimm.py --native --qemu /usr/bin/qemu-system-x86_64 \
  --kernel /path/to/vmlinuz --package /path/to/vram-host.deb --result /tmp/dimm.json
```

The nested fixture can also boot a disposable Linux hypervisor. Omit `--native` and supply `--bios-dir` with QEMU's firmware files. Add `--svm-modules` with the matching outer kernel's module directory to exercise KVM on emulated AMD SVM. Its backing-file filesystem lives inside the outer initramfs. The native file trial uses a preallocated file on the local filesystem. It counts literal reads and writes while enforcing a 64 MiB process address-space limit. Filesystem page cache is accounted by Linux outside this process limit.

```sh
python3 host/tests/file_scale.py --mib 1024 --result /tmp/independent.json
```

Without the FUSE timestamp operation, Linux returned `ENOSYS` from `fsync` after the guest had already written its anonymous allocation. The restored synchronization and independent direct-read checks exercise the missing operation. In an isolated copy, changing the literal slot reader to always read offset zero made `cargo test --locked --offline file_bases_outlive_original_addresses_and_reuse_slots` fail its byte comparison. The restored test covers duplicate bases, a surviving residual, repeated overwrites, slot recycling and a truncated-file read error. [Linux DIMM receipt](../measurements/linux-virtual-dimm.json) · [Independent-page receipt](../measurements/file-backed-independent.json)

## File-backed admission and write rollback in 0.4.1

The native comparison replays 128 MiB per trial on one CPU with fresh preallocated files. Each input contains 32,768 pages. Six pairs alternate which version runs first. Every trial regenerates and compares every byte. Wall times include the filesystem page cache. The backing-read counter counts calls to read a literal slot, including reads satisfied by that cache.

| Input | Median write time, 0.4.0 | Median write time, 0.4.1 | Backing reads during admission, 0.4.0 → 0.4.1 |
| --- | ---: | ---: | ---: |
| Dense pages with sparse edits | 176.7 ms | 70.2 ms | 95,226 → 31,742 |
| Exact repeats | 34.4 ms | 33.5 ms | 32,512 → 32,512 |
| Independent pseudorandom pages | 131.9 ms | 115.0 ms | 0 → 0 |
| Four repeating bases with identical fingerprint samples | 43.2 ms | 45.4 ms | 81,922 → 81,916 |

Both versions admitted 31,742 sparse residuals on the edited input. The new path compares 64-byte rows and applies the candidate's XOR edits to the bytes already read before Verdant checks exact reconstruction and size. An exact match still takes precedence. For independent pages on the fast literal path the file writer now borrows the incoming page buffer. The in-memory store retains an owned copy. The collision control measured a 4.9% increase in median write time. [All paired timings and read counts](../measurements/file-admission.json)

Linking `device/validation/file_admission.c` against the unmodified 0.4.0 library reproduced both regressions. A colliding residual followed by an exact match took eight base reads where four suffice. Sealing the backing file against writes and retrying several new addresses raised store charge from 7,696 to 15,000 bytes. The updated gate requires four reads, exact restores after the original addresses are discarded, admission at 64 edits and literal storage at 65 edits. It also requires the old committed bytes, zero-filled untouched addresses and the original store charge after the failed writes. The device workflow runs this gate against the production library.

To replay the paired trials, build the release library from `c5533c8` separately, build the current device and run:

```sh
python3 device/validation/literal_reuse.py --file-backed \
  --previous-library /path/to/0.4.0/libverdant_memory.a \
  --previous-commit c5533c84284fa55b984768ce127302f21fcb444b --result /tmp/file-admission.json
```

## Owner policy

The inherited Qortara governance and security documents record the accountable owner and security process as `NOT_ESTABLISHED`. The repository-documents check reports these as open owner actions. The architecture index passes. An authorized repository owner must establish those policies.

The Qortara owner-actions check also reports the inherited `external-instruction-lock-8931c560d6673f1b327e` action. Its owner must reconcile independent governance before qualifying protected operations.


## Configured capacity and sparse scaling in 0.5.0

For a configured virtual machine, let P be the platform ceiling in whole MiB, G the guest ceiling bounded by the QEMU process address-space limit, and B the backing byte budget. B includes available filesystem bytes, the process file-size limit and the signed file-offset limit. The policy computes

```text
D = floor(max(0, B − 4096) / 1048576)
C = min(P, G, D)
```

The 4096 bytes preserve the previous literal until its replacement commits. For every natural-number triple P, G, D, the Prism module proves that C satisfies each limit, dominates every other feasible size, and is monotonic in D. When the target min(P,G) fits D, C reaches that target. The installed executable calls the Rust export of the same policy. Replacing the selection of the smaller input with the larger input makes `prism-code check .` reject VR-15 at `CapacityPolicy.admissible`. Restoring it passes the kernel check and vector replay. The launcher also compares the result against the original configured target so a low process limit cannot silently reduce a max request.

These claims concern the supplied bounds. Firmware describes installed-system capabilities, the domain describes a guest configuration, and the backing budget is observed before reservation. QEMU startup, Linux onlining, metadata allocation, memory pressure and failure recovery are systems behaviors. The integration test boots at the computed size with a reserved DIMM and checks ordinary guest accesses after onlining. End-to-end latency depends on the working set, the kernel page cache and the backing device.

The ordered directory has one key per live 64 MiB leaf. An empty extent has constant directory space and initialization work in the advertised capacity. A page lookup costs O(log(L+1)) for L live leaves. Discarding a range seeks to its first live leaf and visits only its K live leaves, with O((K+1) log(L+1)) directory work plus destruction of the affected entries. At most two partial leaves require boundary mask updates. Literal index maintenance is additional. Logical store charge includes live keys, pointers and leaf payloads. RSS also includes tree-node slack and the allocator.

The sparse regression program sets a 64 MiB process address-space limit, writes eight distinct dense pages across leaf boundaries and at both ends of the range, and checks all bytes before and after partial and full discard. Its file-backed variant creates a sparse test file. The packaged VM launcher uses full preallocation. To reproduce after building the host:

```sh
cc -O2 -Wall -Wextra -Werror -Idevice/native device/validation/sparse_extent.c \
  device/target/release/libverdant_memory.a -lpthread -ldl -lm -o /tmp/vram-sparse-extent
/tmp/vram-sparse-extent 1024
/tmp/vram-sparse-extent 1024 file
python3 host/tests/capacity.py
```

The 0.4.1 baseline under the same address-space limit charged 131,256, 1,048,760 and 8,388,792 bytes for empty RAM stores at 1, 8 and 64 TiB. Opening the 1 PiB store failed. The new store charged 184 bytes at all four extents and returned to its starting charge after discard. The sparse file-backed variant charged 256 bytes empty. In a separate, fully reserved 1 GiB file-backed test, all 262,144 independent pages restored exactly at 39,956 KiB peak process RSS. The kernel page cache and the disk reserve have separate costs. [Sparse comparison](../measurements/sparse-extent-0.5.0.json) · [Dense readback](../measurements/dense-reserve-0.5.0.json)

The same 128 MiB file-backed harness ran six paired trials against 0.4.1 on one CPU, alternating implementation order. Times below are medians in milliseconds; every trial checked all 32,768 pages. The increase in read time for the repeated and edited workloads is visible alongside the removal of capacity-dependent startup work. [Raw trials](../measurements/file-latency-0.5.0.json)

| Workload | Write, 0.4.1 | Write, 0.5.0 | Read, 0.4.1 | Read, 0.5.0 |
|---|---:|---:|---:|---:|
| Repeated dense pages | 36.7 | 35.8 | 27.4 | 27.9 |
| Sparse edits to repeated pages | 75.7 | 75.1 | 28.6 | 29.1 |
| Independent pages | 143.2 | 132.3 | 49.1 | 49.1 |
| Four colliding dense bases | 48.2 | 45.2 | 25.7 | 25.8 |

The packaged launcher also booted Linux with a 512 MiB platform ceiling and 320 MiB guest ceiling supplied to `--ram-mib max`. It provided 192 MiB at boot and plugged the reserved 128 MiB while the guest ran. With swap disabled, MemTotal increased by exactly 131,072 KiB and the guest verified 95 MiB on the new DIMM. This run used stock QEMU 10.0.13 and KVM on emulated AMD SVM. [Guest and store counters](../measurements/linux-virtual-dimm-0.5.0.json)

## Dense slot reclamation in 0.5.1

The previous file-backed arena pushed retired slot numbers onto a growable vector. The new arena maintains one availability word per 64 slots reached and links words with available lanes. Both slot selection and return take constant time once the word exists. Growing the word vector on admission has amortized constant cost. The arena uses the existing Prism word-membership and empty-group exports. It commits ownership under its lock only after a complete page write. A final partial word exposes only slots that fit the reserved file.

The release path for a literal slot only updates existing metadata or releases the entire ledger when the last owner drops. This property applies to the slot allocator. The page directory and literal index have their own resource costs. The ledger retains its word capacity until the arena drains, and a completely full arena now pays a small metadata cost before any pages are retired.

On amd64, the targeted test filled 8,193 slots, released the even slots, refilled them and checked every page and the capacity boundary. Against 0.5.0, `cargo test --locked --offline fragmented_release -- --nocapture` failed with `slot reclamation grew metadata`. With 0.5.1 the same test passed. The readings include the arena structure and allocated slot ledger, in bytes.

| Implementation | Empty | Full | Alternating slots released |
|---|---:|---:|---:|
| 0.5.0 | 72 | 72 | 32,840 |
| 0.5.1 | 80 | 4,176 | 4,176 |

The native file-admission test uses a file-size limit to interrupt a 4 KiB write after 1,024 bytes. It checks the partial bytes on disk, restores the previous logical page, and verifies the complete new contents when another page reuses that slot. A separate sealed-file test fills four complete words, forces writes that would require a fifth word to fail, and checks both the previous page and the store charge.

All 262,144 independent pages in a fully reserved 1 GiB store restored under a 64 MiB process address-space limit. In six paired 128 MiB trials the repeated-page workload's median write time rose from 35.6 to 42.1 ms, sparse-edit writes fell from 76.4 to 72.7 ms, and independent writes changed from 130.6 to 131.3 ms. These are workload measurements. [Dense readback](../measurements/dense-reserve-0.5.1.json) · [All paired read and write timings](../measurements/file-latency-0.5.1.json)

The packaged QEMU/KVM test booted with 192 MiB, onlined the reserved 128 MiB DIMM with swap disabled and checked 95 MiB on the added range. [Linux guest receipt](../measurements/linux-virtual-dimm-0.5.1.json)
