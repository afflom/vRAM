"""Compare the installed launcher and ordinary QEMU in fresh memory-accounted systemd services."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
validation = here.parents[1] / "device/validation"
sys.path.insert(0, str(validation))
from reclaim import add, base_entries, save


def receipt(log, prefix):
    rows = re.findall(r"^" + prefix + r" (.+)$", log, re.M)
    if len(rows) != 1:
        raise ValueError(f"missing or duplicate {prefix}: {log[-3000:]}")
    return json.loads(rows[0])


def trial(command):
    # memory.peak belongs to this launch alone and includes charged file cache.
    script = """set -eu
"$@"
group=$(sed -n 's/^0:://p' /proc/self/cgroup)
printf 'CGROUP_PEAK '
cat "/sys/fs/cgroup$group/memory.peak"
"""
    proc = subprocess.run(["systemd-run", "--quiet", "--wait", "--pipe", "--collect",
                           "-p", "MemoryAccounting=yes", "-p", "RuntimeMaxSec=240", "--",
                           "/bin/sh", "-c", script, "measure", *command],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if proc.returncode:
        raise RuntimeError(proc.stdout[-4000:])
    row = receipt(proc.stdout, "PARALLEL_RESULT")
    row["cgroup_peak_bytes"] = receipt(proc.stdout, "CGROUP_PEAK")
    if row["verified_pages"] != 16384 or min(row["write_ns"], row["cgroup_peak_bytes"]) <= 0:
        raise ValueError(f"incomplete measurement: {row}")
    row["write_mib_s"] = 64e9 / row["write_ns"]
    return row, proc.stdout


parser = argparse.ArgumentParser(description=__doc__)
for name in ("qemu", "kernel", "package", "result"):
    parser.add_argument("--" + name, type=Path, required=True)
args = parser.parse_args()
accel = "kvm" if os.access("/dev/kvm", os.R_OK | os.W_OK) else "tcg"
rows = []
with tempfile.TemporaryDirectory() as temp:
    tmp = Path(temp)
    subprocess.run(["dpkg-deb", "-x", args.package, tmp], check=True)
    subprocess.run(["cc", "-O2", "-static", "-pthread", "-Wall", "-Wextra", "-Werror",
                    "-I", validation, here / "parallel_guest.c", "-o", tmp / "init"], check=True)
    guest = base_entries()
    add(guest, "init", tmp / "init", 0o755)
    save(guest, tmp / "guest.gz")
    for cpus in (1, 2, 4, 8):
        options = ["-accel", accel, "-smp", str(cpus), "-kernel", args.kernel.resolve(), "-initrd",
                   tmp / "guest.gz", "-append", "console=ttyS0 quiet rdinit=/init nokaslr panic=-1",
                   "-nodefaults", "-display", "none", "-serial", "stdio", "-no-reboot"]
        for repeat in range(2):
            for backend in (("ordinary", "vram") if repeat == 0 else ("vram", "ordinary")):
                command = ([args.qemu.resolve(), "-m", "384"] if backend == "ordinary" else
                           [tmp / "usr/bin/vram-machine", "run", "--state", tmp / "vm",
                            "--backing-dir", tmp, "--ram-mib", "384", "--qemu", args.qemu.resolve(), "--"])
                row, log = trial([*command, *options])
                if row["vcpus"] != cpus:
                    raise ValueError(f"wrong guest CPU topology: {row}")
                if backend == "vram":
                    row["store"] = receipt(log, "VRAM_FILE")
                rows.append(dict(backend=backend, repeat=repeat, **row))
                print(rows[-1], flush=True)
args.result.write_text(json.dumps(dict(accelerator=accel, host_cpus=len(os.sched_getaffinity(0)),
    kernel=args.kernel.name, qemu=subprocess.check_output([args.qemu, "--version"], text=True).splitlines()[0],
    package_sha256=hashlib.sha256(args.package.read_bytes()).hexdigest(), guest_mib=384,
    workload_mib=64, trials=rows), indent=2) + "\n")
