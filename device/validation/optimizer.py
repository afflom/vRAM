"""Measure verification, total backing, capture and restore CPU across search budgets."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import subprocess

here = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--library", type=Path, required=True)
parser.add_argument("--result", type=Path, required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory() as temp:
    exe = Path(temp) / "optimizer"
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-O2", "-I", here.parent / "native",
                    here / "optimizer.c", args.library, "-lpthread", "-ldl", "-lm", "-o", exe], check=True)
    rows = []
    for trial in range(6):
        order = (0, 1, 7, 49) if trial % 2 == 0 else (49, 7, 1, 0)
        for profile in ("F", "S", "U"):
            for budget in order:
                policies = [0, 1] if budget == 49 else [0]
                for adaptive in (policies if trial % 2 == 0 else reversed(policies)):
                    sample = json.loads(subprocess.check_output([exe, str(budget), profile, str(adaptive)]))
                    rows.append(dict(trial=trial, **sample))
    args.result.write_text(json.dumps(dict(clock="CLOCK_PROCESS_CPUTIME_ID",
        library_sha256=hashlib.sha256(args.library.read_bytes()).hexdigest(), trials=rows), indent=2) + "\n")
