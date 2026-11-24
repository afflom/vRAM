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
    # Each manifest binds the compiled native export to the canonical Prism module.
    for policy, digest in (
        ("host", "204dbd353c4b049a801f4efd22a69cd20f4aac931f40da6d72699016a3c27bb6"),
        ("capacity", "138b38f73224bd3572a7ffa876ce156bc73f9888ed0333a754daeba592abaf2a"),
        ("block", "b083950fd6654115fbde2018f13f0df2d77e4a9663fe82a093c5bf633fc2b57c"),
        ("read", "aa17671a2ffa2129f4bb596efd30bacba14bcac8d76a86b70c6203be61952d40"),
        ("mixed-read", "3e636c7536fadfb81baad9ff2bc8a1acf0fcb996e8eebf62fcbd53d5d9e23058"),
        ("index", "55e68570fb4a1d97c2f04e36c3007bfd1db92a8dad353798b7af725a46f5ca13"),
    ):
        directory = root / "proof" / policy
        raw = (directory / "build-manifest.json").read_bytes()
        manifest = json.loads(raw)
        attestation = json.loads((directory / "bundle/evidence/attestation.json").read_bytes())
        source, = [item for item in manifest["inputs"] if item["kind"] == "source"]
        if (hashlib.sha256(raw).hexdigest() != digest
            or manifest["build_id"] != attestation["build_id"]
            or hashlib.sha256((root.parent / "app" / source["path"]).read_bytes()).hexdigest() != source["sha256"]):
            raise ValueError(f"{policy} policy differs from the certified native export")
    locked = {p["name"]: p for p in tomllib.loads((root / "Cargo.lock").read_text())["package"]}
    for location, pinned in (
        ("proof/capacity/bundle", "5624d595a443a94b2f62d6ca5c8ce2f21caa188a3107599f6cef49f84a741f8f"),
        ("proof/block/bundle", "8bba8b4f185f24bfae921d79809604a247f331aa727ca7ccb98bc6a0630b685b"),
        ("proof/read/bundle", "3f644fb73cc8098276b6fbcfd509cdaf37f2da29180e406e91ac2263eb9a5203"),
        ("proof/mixed-read/bundle", "8db838823ec410c065c13ab700783b60c27bb36c755f3702be89b3fda55fd424"),
        ("proof/index/bundle", "898ba65368d480525b2a742e5080577354dfecc8faea910222de70e945f5cd3f"),
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
