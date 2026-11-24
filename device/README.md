# vRAM for QEMU

vRAM provides an x86 guest’s main memory through QEMU 9.2.0. Ordinary guest reads and writes use resident frames. A host-side optimizer compiles cold pages into byte-exact tensor programs using the released Verdant 2.20.0 tensor core and locally verified LexLean exports. OPTIMIZER.md describes the search, its measured cost, and the TCG and KVM experiments. The older selector-only measurements are in CAPACITY.md.

For the tested CPU-only configuration, select the backend as machine RAM. The following options give a 132 MiB VM a 4 MiB soft frame target, enable the adaptive search and allow up to 49 tile evaluations per eligible capture. The syntax is the same under TCG and KVM.

```sh
-accel kvm -machine pc,memory-backend=mem,mem-merge=off -m 132 \
  -object memory-backend-verdant-tensor,id=mem,size=132M,x-quiescent-reclaim=on,x-resident-target=4M,x-search-nodes=49,x-adaptive-search=on
```

The reclaim option records the operator’s guarantee of exclusive access to managed memory while vCPUs are stopped. Use it only with configurations that exclude asynchronous DMA, pins and external host access to the managed frames. The frame target excludes encoded backing, QEMU, the allocator and kernel overhead. Zero disables the governor. The initializer accepts an optional raw byte image. The host needs Linux userfaultfd with kernel-originating faults enabled, private 4 KiB pages and sufficient memory to service a fault.

## Reproduce

On x86-64 Linux use Rust 1.97.1, Python 3.12+, a C compiler and the usual QEMU
build tools. From this directory, with a fresh QEMU 9.2.0 source release in a
sibling directory, run the following commands. The archive includes the locked Verdant and crates.io sources. The build script verifies the generated package files and builds the Rust library offline.

```sh
python3 build.py
python3 install.py ../qemu-9.2.0
mkdir -p ../qemu-build
(cd ../qemu-build && ../qemu-9.2.0/configure --target-list=x86_64-softmmu --enable-tcg --enable-kvm --disable-docs && ninja qemu-system-x86_64)
```

Select main RAM with `-machine pc,memory-backend=mem,mem-merge=off -object memory-backend-verdant-tensor,id=mem,size=256M,image=initial.bin -m 256`.
The optional image is loaded once, encoded and zero-extended. On a host where
kernel-originating faults are permitted for userfaultfd, the backing values
supply missing pages. RAM operations on resident pages go to the normal CPU
mapping. The device supports private memory with preallocation disabled.

The validation harness boots Linux with a static test init and runs an inner
TCG VM on the new main-RAM backend. This permits real userfaultfd testing on
machines where the outer execution sandbox restricts the syscall. Use a Linux
x86-64 kernel with built-in initramfs, ELF, serial and userfaultfd support.
The recorded runs used Debian's 6.12.117+deb13-amd64 kernel. Download the
scikit-learn digits corpus once. The recorded digest is in results/pager.json.

```sh
curl -fL https://raw.githubusercontent.com/scikit-learn/scikit-learn/main/sklearn/datasets/data/digits.csv.gz -o digits.csv.gz
python3 validation/run.py --qemu ../qemu-build/qemu-system-x86_64 --kernel /path/to/vmlinuz --bios-dir ../qemu-9.2.0/pc-bios --digits digits.csv.gz --result my-run.json
python3 validation/digits.py digits.csv.gz
```

### Earlier corpus evidence

The outer Linux process checked every byte of an 8 MiB mixed region, including
115,008 original digit intensities and 4 MiB of a compressed Linux kernel. It
wrote across page boundaries, checkpointed, verified zero resident pages with
mincore, executed a projection on every cold page and read everything back.
The 2,048 pages faulted in exactly twice in these first two scans. After an
independent high-entropy full overwrite, every page moved to raw representation
and passed a third scan. The two projections each matched 131,072 independent
dense i64 results. The inner guest separately booted with 16 MiB of image-seeded
main RAM, hashed the digits and the kernel prefix through ordinary loads,
and checked cross-page writes and initially zero memory.

### Earlier host-side library measurements

The native classifier trained integer centroids on 1,200 images and evaluated
597 held-out images with both paths. They returned identical predictions
including the same errors. The dense control and packed device both receive
already-laid-out input. Trials exclude training and encoding. Each observation
covers twenty evaluations, and order alternates over seven trials. Times are
host-specific. The 23,952-byte figure counts the page directory and selected
heap objects. Dense padded tiles took 40,960 bytes. Neither figure counts process
overhead, allocator rounding, activations, resident expanded pages or scratch.

### Reclaim inside a Linux guest VM

The experimental `x-vram-reclaim` QMP command takes a backend object ID, an offset **in the memory backend** and a length in bytes. Specify `x-quiescent-reclaim=on` in the memory backend options at startup. The host manager must exclude DMA and all other host access to the selected range. Reclaim accepts paused TCG and capture-all KVM machines with inactive migration. On the tested 192 MiB PC, the guest's allocated pages above the first MiB map directly to backend offsets.

For example, a manager with exclusive access to an 8 KiB range starting at backend offset 9 MiB can issue the following sequence after QMP capability negotiation.

```json
{"execute":"stop"}
{"execute":"x-vram-reclaim","arguments":{"id":"ram","offset":9437184,"length":8192}}
{"execute":"cont"}
```

Wait for each successful response. Reclaim returns the number of populated pages discarded, the number still resident according to `mincore`, and the cumulative number of backend page installations. After an error the manager can inspect the failure and retry the range while the guest remains paused.

Run the paired dirty-page test on x86-64 Linux with a distro Python under `/usr`, static C build support, and a guest kernel with cgroup v2 memory accounting, pagemap, initramfs, serial console and userfaultfd support.

```sh
python3 validation/reclaim.py --qemu ../qemu-build/qemu-system-x86_64 --kernel /path/to/vmlinuz --bios-dir ../qemu-9.2.0/pc-bios --result guest-budget-reclaim.json
```

The outer Linux environment launches two 192 MiB single-vCPU VMs in succession. Each inner QEMU process enters a fresh cgroup before exec, with a 256 MiB memory limit and swap disabled. The control uses ordinary QEMU RAM. The treatment uses vRAM. Both boot the same stock kernel and run the same test process. The process locks its own anonymous pages in guest memory and reports their physical addresses through pagemap so the manager reclaims only the intended frames. It writes a four-byte word straddling each virtual page boundary and verifies every byte after each pause. The final phase updates only boundary words in the previous pseudorandom contents.

The manager checks zero remaining resident pages and retries reclaim before resuming. It then confirms every target page was populated after guest readback. It records process RSS, PSS and swap before reclaim, after reclaim and after readback. The cgroup peak includes allocations charged to the QEMU process from launch. Files already resident in the outer initramfs can be charged elsewhere. Each backend runs once and successive phases reuse the same allocation. The separate automatic-reclaim and KVM experiments are in OPTIMIZER.md.
