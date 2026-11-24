"""Verify independent pages with a preallocated filesystem reserve and a process address-space limit."""
import argparse
import json
import os
from pathlib import Path
import resource
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--result", required=True, type=Path)
parser.add_argument("--mib", type=int, default=256)
args = parser.parse_args()
if args.mib < 64:
    parser.error("test at least 64 MiB")
limit = 64 << 20


def constrain():
    resource.setrlimit(resource.RLIMIT_AS, (limit, limit))


with tempfile.TemporaryFile(dir=args.result.parent) as reserve:
    reserved = (args.mib << 20) + 4096
    os.posix_fallocate(reserve.fileno(), 0, reserved)
    binary = Path(__file__).resolve().parents[1] / "build/vram-scale"
    result = json.loads(subprocess.check_output(
        [binary, str(args.mib), "64", "U", "49", str(reserve.fileno())],
        pass_fds=(reserve.fileno(),), preexec_fn=constrain))
    pages = args.mib * 256
    if (result["verified_pages"] != pages or result["file_base_reads"] < pages
            or result["file_base_writes"] != pages):
        raise RuntimeError(f"incomplete independent-page trial: {result}")
    result.update(reserve_bytes=reserved, address_space_limit_bytes=limit)
    args.result.write_text(json.dumps(result, indent=2) + "\n")
