"""Measure repeated and independent pseudorandom pages against a prior Verdant library."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile

device = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--previous-library", type=Path, required=True)
parser.add_argument("--previous-commit", required=True)
parser.add_argument("--result", type=Path, required=True)
parser.add_argument("--file-backed", action="store_true", help="compare file-backed literal slots")
args = parser.parse_args()
with tempfile.TemporaryDirectory() as temp:
    libraries = {"previous": args.previous_library, "current": device / "target/release/libverdant_memory.a"}
    for name, library in libraries.items():
        subprocess.run(["cc", "-O2", "-I", device / "native", device / "validation/scale.c", library,
                        "-lpthread", "-lm", "-ldl", "-o", Path(temp) / name], check=True)
    rows = []
    profiles = ("R", "D", "U", "C") if args.file_backed else ("R", "D", "U")
    for repeat in range(6 if args.file_backed else 3):
        for profile in profiles:
            for name in (reversed(libraries) if repeat % 2 else libraries):
                cmd = [Path(temp) / name, "128", "256", profile, "49"]
                if args.file_backed:
                    with tempfile.TemporaryFile(dir=args.result.parent) as file:
                        os.posix_fallocate(file.fileno(), 0, (128 << 20) + 4096)
                        row = json.loads(subprocess.check_output(
                            [*cmd, str(file.fileno())], pass_fds=(file.fileno(),)))
                else:
                    row = json.loads(subprocess.check_output(cmd))
                if row["verified_pages"] != 32768 or row["stored_pages"] != 32768:
                    raise ValueError("incomplete 128 MiB readback")
                rows.append(dict(implementation=name, repeat=repeat, **row))
    result = dict(previous_commit=args.previous_commit, kernel=platform.release(), workload_mib=128,
                  repeated_distinct_pages=256, library_sha256={n: hashlib.sha256(p.read_bytes()).hexdigest()
                                                              for n, p in libraries.items()}, trials=rows)
    if args.file_backed:
        result["cpus_allowed"] = sorted(os.sched_getaffinity(0))
        result["backing"] = "preallocated temporary file on the local filesystem"
    args.result.write_text(json.dumps(result, indent=2) + "\n")
