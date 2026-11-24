"""Package the tested static Linux server for installation on Debian and Ubuntu."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

host = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
args = parser.parse_args()
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    for source, dest, mode in [
        ("build/vram-host", "usr/sbin/vram-host", 0o755),
        ("packaging/vram@.service", "usr/lib/systemd/system/vram@.service", 0o644),
        ("packaging/vram.default", "etc/default/vram", 0o644),
        ("packaging/USAGE.md", "usr/share/doc/vram-host/README.md", 0o644),
        ("../LICENSE-MIT", "usr/share/doc/vram-host/copyright", 0o644),
    ]:
        target = root / dest
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(host / source, target)
        target.chmod(mode)
    arch = subprocess.check_output(["dpkg", "--print-architecture"], text=True).strip()
    if arch != "amd64":
        parser.error("package builds are validated on amd64")
    meta = root / "DEBIAN"
    meta.mkdir()
    (meta / "control").write_text(
        "Package: vram-host\nVersion: 0.2.1\nArchitecture: amd64\n"
        "Maintainer: vRAM maintainers <noreply@github.com>\nDepends: systemd, kmod\n"
        "Description: Verdant tensor-backed volatile Linux swap service\n"
        "Homepage: https://github.com/afflom/vRAM\n")
    (meta / "conffiles").write_text("/etc/default/vram\n")
    prerm = meta / "prerm"
    prerm.write_text('#!/bin/sh\nset -e\nif [ "$1" = remove ] && [ -d /run/systemd/system ]; then\n'
                     '    systemctl stop "vram@*.service"\nfi\n')
    prerm.chmod(0o755)
    for name in ("postinst", "postrm"):
        path = meta / name
        path.write_text('#!/bin/sh\nset -e\nif [ -d /run/systemd/system ]; then systemctl daemon-reload; fi\n')
        path.chmod(0o755)
    # mkdtemp honors the builder's umask. dpkg requires traversable package directories.
    for folder in [root, *root.rglob("*")]:
        if folder.is_dir():
            folder.chmod(0o755)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["dpkg-deb", "--root-owner-group", "--build", root, args.output], check=True)
