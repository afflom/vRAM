"""Verify the generated bindings and build the vendored Verdant runtime offline."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import tomllib

root = Path(__file__).resolve().parent


def verify_packages():
    host = root / "proof/host"
    manifest = json.loads((host / "build-manifest.json").read_bytes())
    attestation = json.loads((host / "bundle/evidence/attestation.json").read_bytes())
    source, = [item for item in manifest["inputs"] if item["kind"] == "source"]
    # Bind the model used by the service to the source checked by the root Prism project.
    if (hashlib.sha256((host / "build-manifest.json").read_bytes()).hexdigest() != "204dbd353c4b049a801f4efd22a69cd20f4aac931f40da6d72699016a3c27bb6"
        or manifest["build_id"] != attestation["build_id"]
        or hashlib.sha256((root.parent / "app" / source["path"]).read_bytes()).hexdigest() != source["sha256"]):
        raise ValueError("host policy differs from the certified native export")
    locked = {p["name"]: p for p in tomllib.loads((root / "Cargo.lock").read_text())["package"]}
    for location, pinned in (
        ("proof/host/bundle", "7809b584eaefbb34fb11efe12edf02232d287be713bde40ff1c3d401f0ee26f2"),
        ("proof/search/bundle", "45b658a8b20192b3a99246b765a30bc6258a922199ad7a340e47a0c0c7cba02e"),
        ("proof/bundle", "6ccd8c2fe515cd0b4b5c6321c4a03e12c1692b8a81fa655b3f01f33b765f598c"),
        ("proof/runtime/bundle", "0a59e8e3bbde17b06cf3176130647a3343e3eac4fc2227e925d96c08a6591442"),
    ):
        bundle = root / location
        raw = (bundle / "result.json").read_bytes()
        receipt = json.loads(raw)
        if hashlib.sha256(raw).hexdigest() != pinned:
            raise ValueError(f"unexpected proof bundle in {location}")
        evidence = receipt["verification"]["attestation"]
        if hashlib.sha256((bundle / evidence["path"]).read_bytes()).hexdigest() != evidence["sha256"]:
            raise ValueError(f"verification attestation differs: {location}")
        for entry in receipt["roots"]:
            for output in entry["outputs"]:
                if output["certification"]["status"] != "kernel-checked":
                    raise ValueError(f"uncertified export for {entry['root']}")
                bindings = output["package"]["files"]
                package = Path(bindings[0]["path"]).parts[2]
                lock = locked[package]
                prefix = f'{package}-{lock["version"]}'
                archive = root / "third_party/verdant" / (prefix + ".crate")
                if hashlib.sha256(archive.read_bytes()).hexdigest() != lock["checksum"]:
                    raise ValueError(f"locked archive differs: {package}")
                with tarfile.open(archive, "r:gz") as crate:
                    expected = {str(Path(prefix, *Path(b["path"]).parts[3:])): b for b in bindings}
                    if sorted(crate.getnames()) != sorted(expected):
                        raise ValueError(f"unexpected files in linked package: {package}")
                    for path, binding in expected.items():
                        if hashlib.sha256(crate.extractfile(path).read()).hexdigest() != binding["sha256"]:
                            raise ValueError(f"linked export differs from proof: {path}")
                for binding in bindings:
                    digest = hashlib.sha256((bundle / binding["path"]).read_bytes()).hexdigest()
                    if digest != binding["sha256"]:
                        raise ValueError(f"export differs from verified binding: {binding['path']}")


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--verify-only", action="store_true", help="verify linked generated sources offline")
args = parser.parse_args()
verify_packages()
if args.verify_only:
    raise SystemExit(0)
subprocess.run(["cargo", "build", "--release", "--locked", "--offline"], cwd=root, check=True)
