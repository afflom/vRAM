"""Compare a 32,768-base workload with vRAM 0.2.1, checking each restored byte."""
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
libraries = {"previous": args.previous_library, "current": device / "target/release/libverdant_memory.a"}
rows = []
churn = {}
with tempfile.TemporaryDirectory() as tmp:
    for name, library in libraries.items():
        subprocess.run(["cc", "-O2", "-I", device / "native", device / "validation/scale.c",
                        library, "-lpthread", "-ldl", "-lm", "-o", Path(tmp) / name], check=True)

    for name, library in libraries.items():
        binary = Path(tmp) / (name + "-churn")
        subprocess.run(["cc", "-O2", "-I", device / "native", device / "validation/churn.c",
                        library, "-lpthread", "-ldl", "-lm", "-o", binary], check=True)
        churn[name] = [json.loads(line) for line in subprocess.check_output([binary]).splitlines()]
        if [row["phase"] for row in churn[name]] != list(range(6)):
            raise ValueError("incomplete overwrite and discard trace")

    def trial(name, mib, profile, repeat):
        row = json.loads(subprocess.check_output([Path(tmp) / name, str(mib), "768", profile, "49"]))
        if row["verified_pages"] != row["stored_pages"] or not row["verified_pages"]:
            raise ValueError("incomplete readback")
        if row["stored_pages"] != mib * 256 and row["peak_rss_kib"] <= 768 * 1024:
            raise ValueError("early stop below the RSS budget")
        if name == "current" and profile == "W" and (
            row["verified_pages"] != mib * 256 or row["unique_literals"] != min(mib * 256, 32768)
            or row["literal_hits"] != row["stored_pages"] - row["unique_literals"]
        ):
            raise ValueError("wide dictionary lost a base or exceeded the RSS budget")
        rows.append(dict(implementation=name, repeat=repeat, **row))

    for repeat in range(3):
        for profile, mib in [("W", 128), ("W", 512), ("W", 1024), ("U", 256)]:
            for name in (reversed(libraries) if repeat % 2 else libraries):
                trial(name, mib, profile, repeat)
    for name in libraries:
        trial(name, 65536, "W", 0)

args.result.write_text(json.dumps(dict(previous_commit=args.previous_commit, profile_w_bases=32768,
    rss_budget_mib=768, kernel=platform.release(), arch=platform.machine(),
    library_sha256={k: hashlib.sha256(v.read_bytes()).hexdigest() for k, v in libraries.items()},
    trials=rows, mixed_trace=churn), indent=2) + "\n")
