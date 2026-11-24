"""Measure repeated and independent pseudorandom pages against a prior Verdant library."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import tempfile

device = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--previous-library", type=Path, required=True)
parser.add_argument("--previous-commit", required=True)
parser.add_argument("--result", type=Path, required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory() as temp:
    libraries = {"previous": args.previous_library, "current": device / "target/release/libverdant_memory.a"}
    for name, library in libraries.items():
        subprocess.run(["cc", "-O2", "-I", device / "native", device / "validation/scale.c", library,
                        "-lpthread", "-lm", "-ldl", "-o", Path(temp) / name], check=True)
    rows = []
    for repeat in range(3):
        for profile in ("R", "D", "U"):
            for name in (reversed(libraries) if repeat % 2 else libraries):
                row = json.loads(subprocess.check_output([Path(temp) / name, "128", "256", profile, "49"]))
                if row["verified_pages"] != 32768 or row["stored_pages"] != 32768:
                    raise ValueError("incomplete 128 MiB readback")
                rows.append(dict(implementation=name, repeat=repeat, **row))
    result = dict(previous_commit=args.previous_commit, kernel=platform.release(), workload_mib=128,
                  repeated_distinct_pages=256, library_sha256={n: hashlib.sha256(p.read_bytes()).hexdigest()
                                                              for n, p in libraries.items()}, trials=rows)
    args.result.write_text(json.dumps(result, indent=2) + "\n")
