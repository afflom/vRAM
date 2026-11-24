"""Check firmware parsing, maximum selection and refusal through the installed entry point."""
import errno
import json
import os
from pathlib import Path
import resource
import runpy
import struct
import subprocess
import tempfile
from unittest.mock import patch

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
    globals_ = read_dmi.__globals__
    dmi, boot, record = (root / s for s in ("dmi", "boot", "firmware.json"))
    dmi.write_bytes(legacy + large + end)
    boot.write_text("boot-one")
    with patch.dict(globals_, DMI_TABLE=dmi, BOOT_ID=boot, FIRMWARE_RECORD=record):
        machine["publish_platform"]()
        assert record.stat().st_mode & 0o777 == 0o644
        assert json.loads(record.read_bytes()) == {"platform_max_mib": (4 << 20) + 512, "boot_id": "boot-one"}
        with patch.object(Path, "read_bytes", side_effect=PermissionError):
            assert machine["firmware_capacity"]() == ((4 << 20) + 512, "SMBIOS type 16 (boot record)")
            boot.write_text("boot-two")
            assert machine["firmware_capacity"]() == (None, None)
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

    def no_page():
        resource.setrlimit(resource.RLIMIT_FSIZE, (4095, 4095))

    report = json.loads(subprocess.check_output([cli, "capacity", *options], preexec_fn=no_page))
    assert report["configured_target_mib"] == 4096 and report["feasible_mib"] == 0
    state = root / "state"
    fail = subprocess.run([cli, "run", "--state", state, *options],
                          preexec_fn=no_page, capture_output=True, text=True)
    assert fail.returncode == 1 and "current bound of 0 MiB" in fail.stderr and not state.exists()
    report = json.loads(subprocess.check_output([cli, "capacity", "--platform-max-mib", "4096"]))
    assert report["guest_max_mib"] is None and report["feasible_mib"] is None

    other = root / "second"
    other.mkdir()
    pool = ["--backing-dir", root, "--backing-dir", other, "--platform-max-mib", "4", "--guest-max-mib", "8"]
    report = json.loads(subprocess.check_output([cli, "capacity", *pool], preexec_fn=limit_file))
    assert report["configured_target_mib"] == report["feasible_mib"] == 4
    assert report["backing_shortfall_bytes"] == 0
    assert [(f["files"], f["bytes"], f["max_file_bytes"]) for f in report["reserve_layout"]] == [(2, 4*mib + 4096, 3*mib)]
    stats = os.statvfs(root)
    assert report["filesystem_available_bytes"] <= stats.f_blocks * stats.f_frsize
    dup = [cli, "capacity", *pool[:2], *pool[:2], *pool[4:]]
    rejected = json.loads(subprocess.check_output(dup, preexec_fn=limit_file))
    assert len(rejected["backing_pools"]) == 1 and rejected["reserve_layout"] == report["reserve_layout"]
    # Simulate a quota that statvfs cannot see on the first volume and a
    # filesystem-specific per-file limit on the second. Use real temporary
    # files to check allocation, spillover, and descriptor cleanup on failure.
    args = type("Options", (), {"backing_dir": [root, other]})()
    pools = [dict(directory=str(d), budget_bytes=6*mib, max_file_bytes=6*mib) for d in (root, other)]
    allocate = os.posix_fallocate
    def limited(fd, off, size):
        parent = Path(os.readlink(f"/proc/self/fd/{fd}")).parent
        bound = mib if parent == root else 2*mib
        if off + size > bound:
            raise OSError(errno.EDQUOT if parent == root else errno.EFBIG, "limit")
        allocate(fd, off, size)
    with patch.object(os, "posix_fallocate", limited):
        with machine["open_reserves"](args, pools, 6*mib) as (fds, actual):
            assert [os.fstat(f).st_size for f in fds] == [mib, 2*mib, 2*mib, mib]
            assert [x["bytes"] for x in actual] == [mib, 5*mib]
        try:
            with machine["open_reserves"](args, pools[:1], 2*mib):
                raise AssertionError("partial admission")
        except ValueError as err:
            assert "short by 1048576 bytes" in str(err)
        assert all(not os.path.exists(f"/proc/self/fd/{fd}") for fd in fds)
    for rate, code, verdict in ((0, 0, "complete"), (10**15, 1, "budget")):
        probe = subprocess.run([cli, "qualify", *pool, "--min-read-mib-s", str(rate)],
                               preexec_fn=limit_file, capture_output=True, text=True)
        assert probe.returncode == code, probe.stderr
        r = json.loads(probe.stdout)
        assert r["verdict"] == verdict and r["verified_bytes"] == 4 * mib
        assert r["file_writes"] == 1024 and r["file_reads"] == 16
print("Capacity planner passed boundary, firmware, XML and pre-allocation refusal checks.")
