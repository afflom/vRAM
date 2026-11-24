"""Exercise the installed kernel codec and backing swap in a disposable Linux VM."""
import argparse
import hashlib
import json
import lzma
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "device/validation"))
from run import archive

parser = argparse.ArgumentParser(description=__doc__)
for name in ("qemu", "kernel", "nbd", "bios-dir", "result"):
    parser.add_argument("--" + name, type=Path, required=True)
parser.add_argument("--mib", type=int, default=32)
parser.add_argument("--profile", choices=("S", "M"), default="M")
args = parser.parse_args()
if not 16 <= args.mib <= 8192:
    parser.error("use a workload between 16 and 8192 MiB")
server = root / "host/build/vram-host"
with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    subprocess.run(["cc", "-static", "-O2", f"-DWORK_MIB={args.mib}",
                    f'-DSWAP_SIZE="{args.mib + 64}"', f"-DPROFILE='{args.profile}'", root / "host/tests/init.c", "-o", tmp / "init"], check=True)
    entries = {d: (stat.S_IFDIR | 0o755, b"", 0, 0) for d in ("dev", "sys", "proc")}
    entries["dev/console"] = (stat.S_IFCHR | 0o600, b"", 5, 1)
    for name, data in [("init", (tmp / "init").read_bytes()), ("vram-host", server.read_bytes()),
                       ("nbd.ko", lzma.decompress(args.nbd.read_bytes()))]:
        entries[name] = (stat.S_IFREG | 0o755, data, 0, 0)
    entries["TRAILER!!!"] = (0, b"", 0, 0)
    (tmp / "initrd").write_bytes(archive(entries))
    cmd = [args.qemu, "-accel", "tcg", "-m", str(256 + (args.mib if args.profile == "M" else args.mib // 8)), "-kernel", args.kernel, "-initrd", tmp / "initrd",
           "-append", "console=ttyS0 zswap.enabled=0 quiet rdinit=/init panic=-1", "-L", args.bios_dir, "-nodefaults",
           "-display", "none", "-serial", "stdio", "-device", "isa-debug-exit,iobase=0xf4,iosize=4", "-no-reboot"]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120 + args.mib * 2)
    log = proc.stdout.decode(errors="replace")
    print(log)
    match = re.search(r"VRAM_RESULT (\{[^\n]+\})", log)
    if proc.returncode != 1 or not match:
        sys.exit("Linux swap round trip failed")
    result = json.loads(match[1])
    store = json.loads(re.search(r"VRAM_STORE (\{[^\n]+\})", log)[1])
    if (not store["selector_pages"] or not store["selector_restores"]
            or (args.profile == "M" and not store["raw_pages"])):
        sys.exit("Missing modeled, literal, or restored pages in Verdant")
    result.update(store=store, kernel=args.kernel.name, server_sha256=hashlib.sha256(server.read_bytes()).hexdigest(),
                  kernel_sha256=hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
                  accelerator="TCG", profile=args.profile)
    args.result.write_text(json.dumps(result, indent=2) + "\n")
