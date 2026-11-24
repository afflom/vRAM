"""Benchmark on a Linux host with delegated cgroup v2 memory control and userfaultfd."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile

from capacity_manager import MIB, run_case
from reclaim import build_rom

parser = argparse.ArgumentParser(description=__doc__)
for arg in ("qemu", "bios-dir", "cgroup-parent", "result"):
    parser.add_argument("--" + arg, type=Path, required=True)
parser.add_argument("--accel", choices=("kvm", "tcg"), default="kvm")
parser.add_argument("--profile", choices=("S", "M", "U", "F"), default="S")
parser.add_argument("--maximum-mib", type=int, default=128)
parser.add_argument("--hot-mib", type=int, default=1)
parser.add_argument("--fixed-search", action="store_true", help="disable regional feedback")
parser.add_argument("--search-nodes", type=int, default=49, choices=range(50), metavar="0..49")
parser.add_argument("--resident-mib", type=int, default=4)
parser.add_argument("--budget-mib", type=int, default=64)
parser.add_argument("--reverse", action="store_true")
args = parser.parse_args()
ram_mib = max(132, args.maximum_mib + 4)
if not (1 <= args.hot_mib <= args.maximum_mib <= 1024 and 0 <= args.resident_mib <= ram_mib and args.budget_mib > 0):
    parser.error("invalid guest, hot, resident or budget extent")
if "memory" not in (args.cgroup_parent / "cgroup.subtree_control").read_text().split():
    parser.error("the delegated parent must have the memory controller enabled for its children")

with tempfile.TemporaryDirectory() as temporary:
    tmp = Path(temporary)
    build_rom(tmp, args.maximum_mib)
    cfg = dict(qemu=str(args.qemu.resolve()), bios_dir=str(args.bios_dir.resolve()),
               rom=str(tmp / "capacity.rom"), profile=args.profile, accel=args.accel,
               maximum_mib=args.maximum_mib, hot_mib=args.hot_mib, guest_ram_mib=ram_mib,
               budget_mib=args.budget_mib, search_nodes=args.search_nodes, adaptive=not args.fixed_search, resident_mib=args.resident_mib, geometric=True,
               no_reserve=ram_mib >= 1024)
    rows, outcomes = [], []
    for backend in (("vram", "ordinary") if args.reverse else ("ordinary", "vram")):
        group = Path(tempfile.mkdtemp(dir=args.cgroup_parent, prefix="verdant-vram-"))
        try:
            (group / "memory.max").write_text(str(args.budget_mib * MIB))
            (group / "memory.swap.max").write_text("0")
            points = []
            outcome, attempted = run_case(cfg, backend, group, points)
            rows.extend(points)
            outcomes.append(dict(backend=backend, outcome=outcome, attempted_mib=attempted,
                                 last_fully_verified_mib=points[-1]["verified_mib"] if points else 0,
                                 peak_bytes=int((group / "memory.peak").read_text()),
                                 events=dict(line.split() for line in (group / "memory.events").read_text().splitlines())))
        finally:
            group.rmdir()
    result = dict(acceleration=args.accel, config=cfg, trials=rows, budgets=outcomes,
                  qemu_sha256=hashlib.sha256(args.qemu.read_bytes()).hexdigest(),
                  rom_sha256=hashlib.sha256((tmp / "capacity.rom").read_bytes()).hexdigest())
    args.result.write_text(json.dumps(result, indent=2) + "\n")
