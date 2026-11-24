"""Compare two compiled page stores with the same eight-writer probe and check all readbacks."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--control", type=Path, required=True, help="checkout with a built release library")
parser.add_argument("--result", type=Path, required=True)
args = parser.parse_args()
trials = []
commits = {}
with tempfile.TemporaryDirectory() as tmp:
    binaries = {}
    for name, checkout in (("control", args.control.resolve()), ("current", root)):
        commits[name] = subprocess.check_output(["git", "-C", checkout, "rev-parse", "HEAD"],
                                               text=True).strip()
        binaries[name] = Path(tmp) / name
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-O2", "-I", root / "device/native",
                        root / "device/validation/parallel_store.c",
                        checkout / "device/target/release/libverdant_memory.a",
                        "-lpthread", "-ldl", "-lm", "-o", binaries[name]], check=True)
    for repeat in range(8):
        for version in (("control", "current") if repeat % 2 == 0 else ("current", "control")):
            output = subprocess.check_output([binaries[version], "UDPFA"], text=True)
            rows = [json.loads(line) for line in output.splitlines()]
            if ([r["profile"] for r in rows] != list("UDPFA") or
                    any(r["verified_pages"] != 4096 or min(r["write_ns"], r["read_ns"], r["range_read_p99_ns"]) <= 0 for r in rows)):
                raise ValueError(f"incomplete parallel store trial: {rows}")
            trials.extend(dict(version=version, repeat=repeat, **r) for r in rows)
args.result.write_text(json.dumps(dict(commits=commits, workers=8, mib=16, batch_pages=4,
                                      trials=trials), indent=2) + "\n")
