# Selector-only baseline under a fixed QEMU memory charge

This report describes the earlier selector backend used as a control. OPTIMIZER.md documents the current word kernel, automatic reclaim and adaptive search.

## What a page contains

vRAM inspects the bytes written by the guest. One candidate treats the 4,096 bytes as 64 rows by 64 columns. Every row can have its own pair of arbitrary byte values. A single 64-bit mask specifies which member of each pair supplies each column. There are 128 row-value bytes and 8 column-mask bytes per cold page, plus the page directory and host allocation overhead.

The equivalent integer matrix product has dimensions (64 × 2) × (2 × 64). The right operand has exactly one 1 per column and zeros elsewhere. On a fault, the backend constructs that operand in packed four-bit form, calls Verdant's released `contract_packed_animation_tracks`, and checks that its 4,096 integer outputs fit in bytes. This function executes `uor_matmul::coded_gemm`. Transient buffers include a 32 KiB i64 result. Scratch, CPU time and resident guest frames belong in the host budget.

A local LexLean root, `Vram.Main.matchesCell`, decides whether an observed byte agrees with the selected row value. Its two parametric theorems cover either lane for arbitrary row values and observations. LexLean 0.4.0 and Lean 4.32.1 checked the model and the production Rust export. Cargo links the generated crate directly from the verified bundle. The fitter invokes this root for each cell it checks. QEMU, the page fitter as a whole, the contraction and the pager have separate verification boundaries.

A nonconstant row proposes the mask. Each other row has to agree at every column. The candidate must also reconstruct the original bytes through the actual tensor core and pass Verdant's losslessness and size admission check. Uniform, sparse, palette and raw pages remain available. There is no persisted dense copy in a selector backing object, no cross-page model sharing, and no guest tensor API. Ordinary writes to an expanded page become authoritative. Recapturing it re-evaluates its representation.

The ordinary packed-palette decoder uses a 256-entry nibble lookup built by evaluating Verdant's released `PackedWord.extract` root for both fields of every byte. Initialization checks the published packed-tier specification with `codedTierValid`. The raw copy path uses the released storage-transfer decision. The encoding admission function in the released coded module also performs a Rust byte comparison. These components, the local verification evidence and the Linux/QEMU behavior each have distinct scopes.

## Reading the measurements

A freestanding x86 guest writes every byte of each new 1 MiB tile. Every structured page has page-dependent column selectors and row-dependent pairs. Unstructured pages use a deterministic pseudorandom byte stream. The mixed case alternates the two page types. At a verification checkpoint the guest regenerates and compares every byte ever written. The QEMU manager pauses both arms at the same boundaries. For vRAM, it additionally calls range reclaim, checks the count discarded, and requires zero remaining resident frames in that range. A second series boots a stock Linux kernel and changes a single byte in previously represented pages.

The firmware deliberately reveals the cold ranges. After each growth step it touches one word per page in a moving hot window. During a complete readback it moves through the cold region one MiB at a time, allowing the manager to reclaim that tile before continuing. A large cold working set can fit this way while an application demanding all those bytes simultaneously resident can exhaust the same budget. The tested backend depends on explicit quiescence of CPU and device writers. Automatic cold-page discovery, KVM and device DMA are still open.

Each firmware VM advertises 132 MiB. The outer Linux host creates a fresh QEMU cgroup with a fixed 32 MiB memory limit and zero swap before exec. Files shared with the outer host may be charged elsewhere. The ordinary and vRAM arms use the same firmware binary, addresses, checks and guest traffic in each comparison. A failure counts as a budget outcome only if the inner QEMU process died by SIGKILL and its own cgroup reported an OOM kill. A run that completes the configured ceiling supplies a lower bound. Both run orders are retained. One checkpoint can be well below the first failed allocation because the controller verifies at fixed or geometric steps. For the 128 MiB structured workload the final full scan, including quiescent reclaim and guest-side expected-value regeneration, took 45–51 seconds across the two nested TCG runs. Hardware fault latency and application throughput require separate tests. The JSON includes each fully verified checkpoint, the attempted size at failure and the wall time for the full scan including orchestrated pauses and reclaim. All timings include nested software emulation.

## Observed capacity

Largest fully verified guest allocation in MiB, across two runs with backend order reversed. Two values in a cell indicate the two observed outcomes. The previous vRAM implementation was included in the 128 MiB experiment. Each entry is a checkpoint. The raw JSON also records the attempted size where a cgroup killed a process.

| Page contents | Hot window (MiB) | Ordinary RAM | Previous vRAM | vRAM with selector |
| --- | ---: | ---: | ---: | ---: |
| Structured | 1 | 8 / 16 | 16 | 128 |
| Alternating structured and pseudorandom | 1 | 8 / 16 | — | 40 |
| Pseudorandom | 1 | 8 / 16 | — | 16 |
| Structured | 16 | 8 / 16 | — | 8 |

The structured 1 MiB-window run reached the 128 MiB test ceiling. At that checkpoint 32,512 cold pages held model operands, 256 pages formed the hot window, and the store reported 4,962,352 bytes of backing and directory. The manager also records whole-process cgroup usage. Peak charge and the fully verified checkpoint can vary by run. The 16 MiB hot-window case exposes the cost of keeping frames resident before any eligible cold rows can be reclaimed.

The separate stock Debian Linux trial restored all 4,096 modeled pages in a 16 MiB allocation. The guest changed one byte on every seventeenth page after reclaim. The next capture retained 3,855 modeled pages, changed 241 to raw backing and verified all 16,777,216 bytes.

## Corpus coverage

In the existing mixed 8 MiB pager corpus, the selector representation matched zero pages. That corpus includes 115,008 original digit intensities, a 4 MiB compressed Linux kernel prefix and zero-filled space. The original fill, sparse, palette and raw representations still recovered every byte. The capacity sweeps deliberately vary the fraction of pages that fit the selector model. The useful unknown for a particular application is how many of its live *cold* pages fit, and how often it needs them back.

## Extrapolating the representation

For a fixed address-space directory, each additional cold MiB contains 256 4 KiB pages. If all 256 qualify for the selector representation, the model payload grows by 34 KiB. If all require raw backing, it grows by 1,024 KiB. A half-and-half mix grows by 529 KiB. These are byte counts before allocator rounding. At 1 GiB of entirely modeled cold pages the corresponding payload is 34 MiB. Increasing the advertised address space also grows the directory and population tracking. Hot pages use ordinary 4 KiB frames and may also retain their last backing. The tests measure the full process and its cgroup independently of this calculation.

## Replaying the model evidence

The authoritative model is `proof/project/src/Main.lex.tex`. The local project, lockfile, production request, generated Cargo crate and complete offline replay bundle are included. The tested binary retains Verdant's pinned release dependencies. Building the project checks the generated package file digests recorded in the receipt. With the release of LexLean 0.4.0 and the pinned Lean toolchain available, these commands reproduce the two distinct checks.

```sh
(cd proof/project && lexlean production build --request production.json)
lexlean production verify-artifact proof/bundle
```

The first command runs source verification and named-root extraction. The second checks packaged bindings independently of the source and Lean toolchain. A successful replay reports its own scope. The output for the local root has certification status `kernel-checked`. This receipt covers the generated cell predicate. Whole-page admission additionally runs the real contraction and checks the resulting 4,096 bytes.

## Reproducing the sweep

Use the build and kernel prerequisites from README.md. The `S`, `M` and `U` profiles select structured, mixed and unstructured pages. The compiled binary used for the previous backend can be supplied with `--prior-qemu`. That control executes range reclaim as well. The previous build supports the reclaim command and predates the selector representation.

```sh
python3 validation/reclaim.py --qemu ../qemu-build/qemu-system-x86_64 --kernel /path/to/vmlinuz --bios-dir ../qemu-9.2.0/pc-bios --result measured.json --capacity-profile S --hot-mib 1 --maximum-mib 128 --budget-mib 32 --geometric --reverse --prior-qemu /path/to/pre-selector-qemu
python3 validation/reclaim.py --qemu ../qemu-build/qemu-system-x86_64 --kernel /path/to/vmlinuz --bios-dir ../qemu-9.2.0/pc-bios --result linux-selector.json --linux-selector
```
