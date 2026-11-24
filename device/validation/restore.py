"""Alternate identical native selector reads against two already built static libraries."""
import argparse
import hashlib
import json
import platform
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--previous", type=Path, required=True)
parser.add_argument("--current", type=Path, required=True)
parser.add_argument("--result", type=Path, required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    digests = {}
    for name, library in (("previous", args.previous), ("current", args.current)):
        subprocess.run(["cc", "-O3", "-Wall", "-Wextra", "-Werror", "-I", here.parent/"native",
                        here/"restore.c", library, "-lm", "-lpthread", "-ldl", "-o", tmp/name], check=True)
        digests[name] = hashlib.sha256(library.read_bytes()).hexdigest()
    trials = []
    for pair in range(8):
        order = ("previous", "current") if pair % 2 == 0 else ("current", "previous")
        for name in order:
            sample = json.loads(subprocess.check_output([tmp/name]))
            trials.append(dict(pair=pair, version=name, **sample))
    args.result.write_text(json.dumps(dict(clock="CLOCK_PROCESS_CPUTIME_ID", machine=platform.machine(),
                                          library_sha256=digests, trials=trials), indent=2) + "\n")
