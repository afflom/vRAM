# vRAM verification

The root Prism project defines capacity decisions and pilot selection. Build and verify the checkout with Prism Code 0.11.0. The model carries nineteen theorems and twenty executable acceptance vectors. The project verifier checks the built holo against the model on disk and replays its vectors. The artifact reports evidence level P0. Its project verification covers identity and replay. It carries no separately signed release attestation.

The runtime uses the published Verdant tensor crate and ten local LexLean exports packaged in the locked offline Cargo registry. `python3 device/build.py --verify-only` verifies the four export receipt digests and their attestations, kernel-checked status, every registered binding, and the archive contents actually linked by Cargo. The host transport passes writes, reads and discards to this runtime. NBD negotiation, Linux reclaim, memory locking, the allocator, page I/O and the measurement harness are systems code exercised by integration tests. The proofs cover their named model definitions.

## Fault injection

Changing the failed-readback branch of `decision` from halt to advance made `prism-code check .` exit 1. It reported `contract.theorem-unproved` for VR-02 and `proof.rejected` for `failedBytesStop`. Restoring the model passed the project check, build and vector replay.

Appending one byte to the locked `vram_word` archive in a temporary checkout made `python3 device/build.py --verify-only` exit 1 with `locked archive differs: vram_word`. The unaltered archive passed. A Linux test that wrote and immediately reread anonymous pages produced 8,192 swap writes but zero Verdant restores. The integration gate rejected it. The current test applies cgroup pressure, checks actual swap reads and selector restores, and compares every byte.

The multi-GiB fixture budgets for page tables when it forces reclaim. It releases the byte-verified mapping before invoking the detach command. With the 2 GiB mapping still present and swap already disabled, Linux refused that fork with `ENOMEM`. The final fixture requires successful readback, swapoff, server exit and detach before writing a passing receipt.

## Managed host and cross-page reuse

The canonical host model is `app/src/HostPolicy.lex.tex`. The native Rust crates for autosizing and empty-group release came from a kernel-checked production build of that same file. The build verifies the source against the production input manifest. To reproduce with the pinned LexLean compiler, copy `device/proof/host/{lexlean.toml,lexlean.lock,lean-toolchain,lakefile.toml,lake-manifest.json,production.json}` to an empty project and the canonical source to its `src/`, then run `lexlean check` and `lexlean production build --request production.json`.

A planted rule that released every group failed the Prism theorem `HostPolicy.retainOccupied`. The modified source also failed the offline runtime verifier with `host policy differs from the certified native export`. Suppressing XOR application in the dense residual decoder failed the collision, overwrite and discard regression. The restored gates pass. A sparse directory regression exercises the first and final pages of a 1 TiB logical store, partial discard across a group boundary and release of the last occupied leaf. It checks that empty metadata costs under 256 KiB and returns to its original size after discard.

The Debian package installs disabled. The managed Linux fixtures require a readiness datagram after real swapon, positive kernel swap traffic, all bytes restored, live engine counters and termination through successful swapoff. A live 1 MiB anonymous range is checked again after service exit. The R, D and U fixtures exercise exact sharing, dense residuals and independent literal writes respectively. Reproduce with `host/tests/boot.py --managed --profile D` and the kernel, NBD module, QEMU and output arguments in the README.

The distribution fixture installs the package in a disposable Debian 13 amd64 generic cloud image, loads the distribution NBD driver, exercises SQLite under kernel reclaim and checks that swap reactivates at the next boot. With QEMU and genisoimage installed, run `python3 host/tests/distribution.py --image debian-13-generic-amd64.qcow2 --deb vram-host_0.2.0_amd64.deb --result debian-installed.json`. QEMU uses a temporary writable overlay. The receipt identifies the image and package by SHA-256. [Debian receipt](../measurements/debian-installed.json)

To repeat the native comparison, build the previous library from commit `0d24329` and the current one, then run `python3 device/validation/literal_reuse.py --previous-library /path/to/libverdant_memory.a --previous-commit 0d24329 --result comparison.json`. Times include input generation on the same machine and should be compared within a run.

## Owner policy

The inherited Qortara governance and security documents record the accountable owner and security process as `NOT_ESTABLISHED`. The repository-documents check reports these as open owner actions. The architecture index passes. An authorized repository owner must establish those policies.

The Qortara owner-actions check also reports the inherited `external-instruction-lock-8931c560d6673f1b327e` action. Its owner must reconcile independent governance before qualifying protected operations.
