# vRAM verification

The root Prism project defines capacity decisions and pilot selection. Build and verify the checkout with Prism Code 0.11.0. The model carries ten theorems and ten executable acceptance vectors. The project verifier checks the built holo against the model on disk and replays its vectors. The artifact reports evidence level P0. Its project verification covers identity and replay. It carries no separately signed release attestation.

The runtime uses the published Verdant tensor crate and eight local LexLean exports packaged in the locked offline Cargo registry. `python3 device/build.py --verify-only` verifies the three export receipt digests, kernel-checked status, every registered binding, and the archive contents actually linked by Cargo. The host transport passes writes, reads and discards to this runtime. NBD negotiation, Linux reclaim, memory locking, the allocator, page I/O and the measurement harness are systems code exercised by integration tests. The proofs cover their named model definitions.

## Fault injection

Changing the failed-readback branch of `decision` from halt to advance made `prism-code check .` exit 1. It reported `contract.theorem-unproved` for VR-02 and `proof.rejected` for `failedBytesStop`. Restoring the model passed the project check, build and vector replay.

Appending one byte to the locked `vram_word` archive in a temporary checkout made `python3 device/build.py --verify-only` exit 1 with `locked archive differs: vram_word`. The unaltered archive passed. A Linux test that wrote and immediately reread anonymous pages produced 8,192 swap writes but zero Verdant restores. The integration gate rejected it. The current test applies cgroup pressure, checks actual swap reads and selector restores, and compares every byte.

## Owner policy

The inherited Qortara governance and security documents record the accountable owner and security process as `NOT_ESTABLISHED`. The repository-documents check reports these as open owner actions. The architecture index passes. An authorized repository owner must establish those policies.

The Qortara owner-actions check also reports the inherited `external-instruction-lock-8931c560d6673f1b327e` action. Its owner must reconcile independent governance before qualifying protected operations.
