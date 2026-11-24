"""Verify independent or mixed pages with a preallocated filesystem reserve and a process address-space limit."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--result", required=True, type=Path)
parser.add_argument("--mib", type=int, default=256)
parser.add_argument("--control", type=Path, help="compare a built checkout in eight alternating pairs")
parser.add_argument("--profile", choices=["U", "M"], default="U",
                    help="independent literals or alternating selectors and independent literals")
args = parser.parse_args()
if args.mib < 64:
    parser.error("test at least 64 MiB")
limit = 64 << 20


def constrain():
    resource.setrlimit(resource.RLIMIT_AS, (limit, limit))


root = Path(__file__).resolve().parents[2]
current = root / "host/build/vram-scale"


def trial(binary, coalesced):
    with tempfile.TemporaryFile(dir=args.result.parent) as reserve:
        reserved = (args.mib << 20) + 4096
        os.posix_fallocate(reserve.fileno(), 0, reserved)
        row = json.loads(subprocess.check_output(
            [binary, str(args.mib), "64", args.profile, "49", str(reserve.fileno()), "range"],
            pass_fds=(reserve.fileno(),), preexec_fn=constrain))
    pages = args.mib * 256
    literals = pages if args.profile == "U" else pages // 2
    reads = row["file_base_reads"] - row["admission_base_reads"]
    if (row["verified_pages"] != pages or row["range_pages"] != 64 or row["range_seconds"] <= 0
            or row["file_base_writes"] != literals or not pages // 64 <= reads <= literals
            or (coalesced and reads != pages // 64)):
        raise RuntimeError(f"incomplete {args.profile} range trial: {row}")
    return dict(row, reserve_bytes=reserved, address_space_limit_bytes=limit)


if args.control:
    control = args.control.resolve()
    lib = "device/target/release/libverdant_memory.a"
    control_commit = subprocess.check_output(["git", "-C", control, "rev-parse", "HEAD"], text=True).strip()
    hashes = {name: hashlib.sha256((checkout / lib).read_bytes()).hexdigest()
              for name, checkout in (("control", control), ("current", root))}
    trials = []
    with tempfile.TemporaryDirectory() as tmp:
        baseline = Path(tmp) / "control-scale"
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-O2", "-I", root / "device/native",
                        root / "device/validation/scale.c", control / lib, "-lpthread", "-ldl", "-lm",
                        "-o", baseline], check=True)
        for repeat in range(8):
            for name in (("control", "current") if repeat % 2 == 0 else ("current", "control")):
                row = trial(current if name == "current" else baseline, name == "current")
                trials.append(dict(row, version=name, repeat=repeat))
    result = dict(control_commit=control_commit, library_sha256=hashes, trials=trials)
else:
    result = trial(current, True)
args.result.write_text(json.dumps(result, indent=2) + "\n")
