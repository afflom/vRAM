"""Check firmware parsing, maximum selection and refusal through the installed entry point."""
import json
from pathlib import Path
import resource
import runpy
import struct
import subprocess
import tempfile

host = Path(__file__).resolve().parents[1]
machine = runpy.run_path(str(host / "vram-machine"))
read_dmi = machine["dmi_capacity"]
mib = 1 << 20


def array(handle, kib, extended=0):
    record = bytearray(23)
    struct.pack_into("<BBH", record, 0, 16, len(record), handle)
    record[5] = 3
    struct.pack_into("<I", record, 7, kib)
    struct.pack_into("<Q", record, 15, extended)
    return bytes(record) + b"\0\0"


legacy = array(1, 512 * 1024)
large = array(2, 0x80000000, 4 << 40)
end = bytes([127, 4, 255, 255, 0, 0])
assert read_dmi(legacy + large + end) == (4 << 20) + 512
for invalid in (legacy, legacy + end[:-1], legacy + legacy + end, array(3, 0) + end):
    assert read_dmi(invalid) is None

with tempfile.TemporaryDirectory() as folder:
    root = Path(folder)
    cli = root / "bin/vram-machine"
    cli.parent.mkdir()
    cli.write_bytes((host / "vram-machine").read_bytes())
    cli.chmod(0o755)
    helper = root / "lib/vram/vram-ram-file"
    helper.parent.mkdir(parents=True)
    helper.symlink_to(host / "build/vram-ram-file")
    for n in [1, 3, 1 << 20, 3 << 20, ((1 << 63) - 4097) // mib]:
        for delta in (-1, 0, 1):
            output = subprocess.check_output([helper, "--ceiling", str(n + 1), str(n),
                                              str(n * mib + 4096 + delta)])
            assert int(output) == n - (delta < 0)
    xml = root / "domain.xml"
    xml.write_text('<domain><memory>2048</memory><maxMemory unit="GiB">4</maxMemory></domain>')
    options = ["--backing-dir", root, "--platform-max-mib", "8192", "--domain-xml", xml]

    def limit_file():
        resource.setrlimit(resource.RLIMIT_FSIZE, (3 * mib + 4095, 3 * mib + 4095))

    report = json.loads(subprocess.check_output([cli, "capacity", *options], preexec_fn=limit_file))
    assert report["configured_target_mib"] == 4096 and report["feasible_mib"] == 2
    state = root / "state"
    fail = subprocess.run([cli, "run", "--ram-mib", "max", "--state", state, *options],
                          preexec_fn=limit_file, capture_output=True, text=True)
    assert fail.returncode == 1 and "current bound of 2 MiB" in fail.stderr and not state.exists()
    report = json.loads(subprocess.check_output([cli, "capacity", "--platform-max-mib", "4096"]))
    assert report["guest_max_mib"] is None and report["feasible_mib"] is None
print("Capacity planner passed boundary, firmware, XML and pre-allocation refusal checks.")
