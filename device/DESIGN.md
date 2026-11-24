# vRAM as guest physical memory

The byte contract below also applies to the current implementation. The implementation history and proposed tests on this page predate the adaptive optimizer. OPTIMIZER.md specifies the current capture, fault and reclaim paths.

The device under test is the QEMU machine's main RAM. An unmodified guest OS allocates memory, runs applications, writes arbitrary bytes and reads them back through ordinary addresses. The vRAM backend owns the host resources that implement those addresses. Verdant operates inside that backend. Guest allocations carry no assumptions about shape, type, compressibility or how long a page will stay cold.

## Byte contract and proposed implementation

A resident page is an ordinary CPU-accessible frame. CPU caches and guest memory ordering govern accesses. A cold page can be stored in any representation from which the backend can reconstruct all its bytes. A guest read or write to a missing page triggers restoration of the complete current page, including bytes outside the requested address range. An overwrite crossing a page boundary must preserve the unaffected bytes on both sides.

The current store supports uniform bytes, a fill plus sparse exceptions, packed four-bit palette entries plus exact exceptions, raw bytes, and an exact 64-row model with a shared binary column selector. CAPACITY.md details the model, its LexLean admission rule and the fixed-budget runs. It calls Verdant's released representation-admission logic when considering encoded candidates. A proposed deterministic model-backed representation would also have to retain and charge every input needed to reconstruct the page. An ordinary guest write supersedes the old value whatever its original representation. High-entropy pages can cost their full logical size plus metadata.

Prism's role is to specify and establish properties of these transformations. For every admitted representation r of page p, decode(r) must equal p byte for byte. For reclamation, the restored bytes must equal the last value committed while writers were excluded. The existing upstream predicates and tests provide a starting point. The QEMU and Linux synchronization that makes a particular version safe to capture is a separate implementation obligation.

## Reclaiming a guest-written page

1. Start with a controlled reclaim point. Quiesce vCPUs and host writers, drain or exclude device DMA, and pin the selected page. Read its authoritative resident bytes.
2. Encode the page and commit its backing. If allocation or encoding fails, retain the valid resident page. Discard the resident mapping only after a complete backing value is available.
3. Resume the VM. On a fault, restore the committed version into a reserved frame before waking the access. Concurrent faults for one page must agree on that version.
4. Add online reclaim once the controlled transition passes. Track dirty state and ownership across vCPUs, DMA and host aliases. Keep pinned pages resident and reserve enough capacity to service faults under pressure. Migration and snapshots must carry authoritative dirty bytes and dependencies of encoded pages.

The QEMU backend exposes the experimental QMP command `x-vram-reclaim`. A manager supplies a backend object ID, byte offset and length. Both range boundaries must be 4 KiB aligned. The backend must have opted into quiescent reclaim at construction, the virtual CPUs must be paused under TCG and migration must be inactive. The operator must exclude DMA and other host access to the range throughout the call. A paused VM alone cannot establish that condition for arbitrary devices.

The pager encodes each populated page before discarding its frame, reports the number discarded, and checks residency using Linux `mincore`. A retry on an already reclaimed page skips it. If a later page fails, the VM remains paused and earlier committed pages can still fault back in. The manager resumes the VM explicitly. Code that reads the restored pages uses ordinary memory operations. The current backend aborts the process on an unrecoverable fault-service error. Production recovery and a fault-service resource reserve remain open.

## What the existing runs establish

A 16 MiB guest using this backend as main RAM read an image with digit intensities and a kernel prefix, verified hashes, and checked initially zero memory and cross-page writes under nested QEMU TCG. Separately, an 8 MiB pager test reclaimed all 2,048 pages and verified every byte after restoration. Its mixed input occupied 4,295,244 accounted backing and directory bytes. A complete pseudorandom overwrite selected 2,048 raw pages and occupied 8,421,416 bytes for 8,388,608 logical bytes. These figures omit resident frames and other host costs. The archive's packed projection and classifier runs exercise the underlying library. Device acceptance depends on the RAM tests.


A stock Debian 6.12.117 Linux guest has now completed the QEMU reclaim test. The same test process verified ordinary anonymous memory on both backends, from a two-page boundary write up to a 4 MiB region. On vRAM all six paused reclaim operations (one per workload phase, one or more QMP requests per range set) left zero target pages resident. Retrying the ranges before resume discarded zero additional pages. On readback the fault counts increased by exactly 2, 256, 1,024, 1,024, 1,024 and 1,024. The guest verified every byte, including the untouched contents after patching the preceding pseudorandom region.

Both 192 MiB VMs ran under the same 256 MiB QEMU cgroup limit with swap disabled, using nested TCG and one inner vCPU. Cgroup peaks were 191.3 MiB for ordinary RAM and 184.6 MiB for vRAM in this single pair, ordinary first. Neither reached the limit. For each 4 MiB range, QEMU process PSS fell 4 MiB immediately after vRAM reclaim and rose 4 MiB on full readback. The observation held for sparse changes, palette data and pseudorandom bytes. Each phase reused the same process and allocation. These deltas describe resident frames within a process that also keeps backing and allocator capacity. A compression ratio or maximum sustainable guest size requires separate measurements. The paired measurements are in `results/guest-budget-reclaim.json`.

## How to test whether vRAM scales

Run the same unmodified guest kernel, image, vCPU configuration, advertised RAM and workload on the same KVM host. Change only the main-memory backend across paired trials. Start with QEMU's ordinary RAM backend as the control. Apply the same total physical-memory limit and swap policy, including all helper processes. Record any disk spill. Count resident guest frames, encoded backing, metadata, page tables, scratch and reserves in the host budget, measuring actual peak usage as well as individual components.

Vary the number of distinct pages written independently of the address space advertised. At each size, test sparse and repetitive writes, independently generated high-entropy writes, and a mixed workload with a changing hot set. Write per-page versioned data from the guest and verify it after reclaim. Measure verified live guest bytes sustained at each host budget, peak host memory, wall time, host CPU and p50/p99 fault service latency. Repeat runs and report variation. A capacity improvement is a larger verified working set under the same host limit at a stated throughput and latency. A loss under high-entropy writes belongs on the same curve.

[Verdant Tensor source](https://github.com/afflom/verdant-engine/tree/v2.20.0/crates/verdant-tensor/src) · [PrismPM](https://github.com/UOR-Foundation/prismpm) · [Linux userfaultfd](https://docs.kernel.org/admin-guide/mm/userfaultfd.html)
