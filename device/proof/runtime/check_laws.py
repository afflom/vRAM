"""Replay supplemental laws against exactly the Lean modules in the attestation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--search", action="store_true")
runtime = Path(__file__).resolve().parent
root = runtime.parent / "search" if parser.parse_args().search else runtime
receipt = json.loads((root / "bundle/evidence/attestation.json").read_bytes())
expected = {item["module"]: item["sha256"] for item in receipt["oleans"]}
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "lean"
    shutil.copytree(root / "lean", source)
    if "LexLeanRuntime.Portable" in expected and not (source / "LexLeanRuntime").exists():
        shutil.copytree(runtime / "lean/LexLeanRuntime", source / "LexLeanRuntime")
    shutil.copyfile(root / "Laws.lean", source / "Laws.lean")
    env = dict(os.environ, LEAN_PATH=str(source))
    for module in ("LexLeanRuntime.Portable", "Vram.Main"):
        if module not in expected:
            continue
        stem = Path(*module.split("."))
        output = stem.with_suffix(".olean")
        subprocess.run(["lean", "-o", output, stem.with_suffix(".lean")], cwd=source, env=env, check=True)
        if hashlib.sha256((source / output).read_bytes()).hexdigest() != expected[module]:
            raise ValueError(f"{module} differs from the production attestation")
    subprocess.run(["lean", "Laws.lean"], cwd=source, env=env, check=True)
