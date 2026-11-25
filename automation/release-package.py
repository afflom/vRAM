"""Keep the exact published deb when a rebuild differs only in archive metadata."""
from hashlib import sha256
from io import BytesIO
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile


BASE = "https://afflom.github.io/vRAM/downloads/"
FETCH = ["curl", "--fail", "--silent", "--show-error", "--location", "--retry", "3"]


def contents(deb, section):
    data = subprocess.check_output(["dpkg-deb", section, deb])
    with tarfile.open(fileobj=BytesIO(data)) as archive:
        entries = {}
        for entry in archive:
            if entry.name in entries:
                raise ValueError(f"duplicate {entry.name} in {deb}")
            digest = sha256(archive.extractfile(entry).read()).digest() if entry.isfile() else None
            entries[entry.name] = (entry.type, entry.mode, entry.uid, entry.gid, entry.linkname,
                                   entry.devmajor, entry.devminor, {k: v for k, v in entry.pax_headers.items() if k != "mtime"},
                                   digest)
        return entries


def sums(text):
    packages = {}
    for line in text.splitlines():
        checksum, name = line.split()
        if (not re.fullmatch(r"vram-(?:host|kernel-dkms)_[0-9A-Za-z.+~-]+_amd64\.deb", name)
                or not re.fullmatch(r"[0-9a-f]{64}", checksum) or name in packages):
            raise ValueError(f"invalid published package entry: {line}")
        packages[name] = checksum
    return packages


def download(path, digest):
    subprocess.run([*FETCH, BASE + path.name + "?sha256=" + digest, "-o", path], check=True)
    if sha256(path.read_bytes()).hexdigest() != digest:
        raise ValueError(f"published checksum mismatch for {path.name}")


def retain(package, previous):
    for section in ("--ctrl-tarfile", "--fsys-tarfile"):
        old = contents(previous, section)
        new = contents(package, section)
        for name in sorted(old.keys() | new.keys()):
            if old.get(name) != new.get(name):
                raise ValueError(f"{package.name} changed {name} in {section}; bump its version")
    previous.replace(package)


def main():
    destination = Path(sys.argv[1])
    published = sums(subprocess.check_output([*FETCH, BASE + "SHA256SUMS"], text=True))
    if destination.is_dir():
        # Preserve versioned links when the Pages deployment replaces the site.
        manifest = destination / "SHA256SUMS"
        current = sums(manifest.read_text())
        for name, digest in published.items():
            if name not in current:
                download(destination / name, digest)
                current[name] = digest
            elif current[name] != digest:
                raise ValueError(f"refusing to replace published {name}")
        manifest.write_text("".join(f"{digest}  {name}\n" for name, digest in sorted(current.items())))
    elif destination.name in published:
        digest = published[destination.name]
        with tempfile.TemporaryDirectory(dir=destination.parent) as tmp:
            previous = Path(tmp) / destination.name
            download(previous, digest)
            retain(destination, previous)
        print(f"Preserved published {destination.name} ({digest})")


if __name__ == "__main__":
    main()
