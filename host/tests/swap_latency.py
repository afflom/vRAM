"""Compare queue-depth-one 4 KiB block I/O on fresh zram and vRAM devices in a disposable VM."""
import argparse
import hashlib
import json
import lzma
from pathlib import Path
import re
import stat
import statistics
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "device/validation"))
from run import archive

parser = argparse.ArgumentParser(description=__doc__)
for name in ("qemu", "kernel", "modules", "control", "result"):
    parser.add_argument("--" + name, type=Path, required=True)
parser.add_argument("--candidate", type=Path, default=root / "host/build/vram-host")
parser.add_argument("--bios-dir", type=Path)
parser.add_argument("--accel", choices=["kvm", "tcg"], required=True)
parser.add_argument("--require-parity", action="store_true", help="exit unsuccessfully if any median exceeds zram")
args = parser.parse_args()
servers = {key: getattr(args, key).read_bytes() for key in ("control", "candidate")}
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    subprocess.run(["cc", "-static", "-O2", "-Wall", "-Wextra", "-Werror",
                    root / "host/tests/swap_latency_guest.c", "-o", path / "init"], check=True)
    files = dict(servers, init=(path / "init").read_bytes())
    for name in ("nbd", "zram", "lz4_compress", "lz4hc_compress"):
        module, = args.modules.rglob(name + ".ko.xz")
        files[name + ".ko"] = lzma.decompress(module.read_bytes())
    entries = {d: (stat.S_IFDIR | 0o755, b"", 0, 0) for d in ("dev", "sys", "proc")}
    entries["dev/console"] = (stat.S_IFCHR | 0o600, b"", 5, 1)
    entries.update({k: (stat.S_IFREG | 0o755, v, 0, 0) for k, v in files.items()})
    entries["TRAILER!!!"] = (0, b"", 0, 0)
    (path / "initrd").write_bytes(archive(entries))
    command = [args.qemu, "-accel", args.accel, "-smp", "2", "-m", "384", "-kernel", args.kernel,
               "-initrd", path / "initrd", "-nodefaults", "-display", "none", "-serial", "stdio",
               "-device", "isa-debug-exit,iobase=0xf4,iosize=4", "-no-reboot",
               "-append", "console=ttyS0 quiet zswap.enabled=0 rdinit=/init panic=-1"]
    if args.bios_dir:
        command.extend(["-L", args.bios_dir])
    if args.accel == "kvm":
        command.extend(["-cpu", "host"])
    proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
    log = proc.stdout.decode(errors="replace")
    if proc.returncode != 1:
        sys.exit(log[-4000:])

rows = {}
for run, backend, profile, op, p50, p99 in re.findall(
        r"^PAGE_LATENCY (\d+) (\S+) ([SU]) (read|write) (\d+) (\d+)\s*$", log, re.M):
    key = (int(run), backend, profile, op)
    if key in rows or not 0 < int(p50) <= int(p99):
        sys.exit("Invalid timing observation: " + repr(key))
    rows[key] = dict(p50_ns=int(p50), p99_ns=int(p99))
if len(rows) != 36:
    sys.exit("Incomplete timing matrix\n" + log[-4000:])
summary = {}
for workload, profile in (("patterned", "S"), ("dense", "U")):
    for op in ("write", "read"):
        times = {b: [rows[i,b,profile,op] for i in range(3)] for b in ("zram-zstd", "control", "vram")}
        medians = {b: statistics.median(r["p50_ns"] for r in samples) for b, samples in times.items()}
        summary[f"{workload}_{op}"] = dict(runs=times, median_ns=medians,
            vram_to_zram=medians["vram"]/medians["zram-zstd"], meets_zram=medians["vram"] <= medians["zram-zstd"])
result = dict(accelerator=args.accel, kernel=args.kernel.name, kernel_sha256=hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
              server_sha256={k: hashlib.sha256(v).hexdigest() for k,v in servers.items()},
              samples_per_run=1024, page_bytes=4096, queue_depth=1, runs=3,
              verified_bytes_per_backend=3*2*1024*4096, meets_parity=all(v["meets_zram"] for v in summary.values()),
              workloads=summary)
args.result.write_text(json.dumps(result, indent=2) + "\n")
print({k:v["median_ns"] for k,v in summary.items()})
if args.require_parity and not result["meets_parity"]:
    sys.exit("Single-page zram parity not reached: " + ", ".join(k for k,v in summary.items() if not v["meets_zram"]))
