"""Replay native sizing through the planner and Prism policy in the shipping deb."""
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import tempfile
from unittest.mock import patch

with tempfile.TemporaryDirectory() as tmp:
    subprocess.run(["dpkg-deb", "-x", sys.argv[1], tmp], check=True)
    lib = Path(tmp, "usr/lib/vram")
    policy = (lib / "kernel-size.policy").read_bytes()
    planner = runpy.run_path(str(lib / "kernel-plan"))
    machine = runpy.run_path(str(lib / "kernel-firmware.py"))
    record = bytearray(15)
    struct.pack_into("<BBH", record, 0, 16, 15, 1)
    record[5] = 3
    struct.pack_into("<I", record, 7, 16 << 20)
    dmi = Path(tmp, "DMI")
    dmi.write_bytes(record + b"\0\0\x7f\x04\xff\xff\0\0")
    firmware = machine["firmware_capacity"]
    architecture = planner["architecture_limit"]
    four = "address sizes : 48 bits physical, 48 bits virtual\nflags : sse2"
    assert architecture(four, "") == 1 << 26
    assert architecture(four + " la57", "") == 1 << 28
    assert architecture(four + " la57", "no5lvl") == 1 << 26
    assert architecture(four, "quiet mem=8G -- mem=1G") == 8192
    for value in ("8G", "010G", "0x8G", "0X2000m", "8589934592"):
        assert architecture(four, f"quiet mem={value}") == 8192, value
    assert architecture(four, "mem=010G mem=0x4G -- mem=1G") == 4096
    assert architecture("", "") is None

    def size(mem_kib, swap_kib=0, **env):
        return planner["plan"](env, {"MemTotal": mem_kib, "SwapTotal": swap_kib}, firmware, policy)[0]

    with patch.dict(firmware.__globals__, DMI_TABLE=dmi):
        ram = 8 << 20
        assert size(ram, 1024) == 8192
        assert size(ram, 1028) == 8191
        assert size(16 << 20) == 0
        assert size((16 << 20) - 4) == 1
        assert size(ram, VRAM_OS_MAX_MIB="4096") == 0
        assert size(ram, VRAM_PLATFORM_MAX_MIB="12288", VRAM_OS_MAX_MIB="10240") == 2049
        limit = planner["DEVICE_MAX_MIB"]
        assert size(4, VRAM_PLATFORM_MAX_MIB=str(limit)) == limit
        assert size(4, VRAM_PLATFORM_MAX_MIB=str(limit + 1)) == limit
        dmi.unlink()
        assert size(ram) == 64
        assert size(ram, VRAM_OS_MAX_MIB="4096") == 0
        assert size(ram, VRAM_KERNEL_SIZE_MIB="96") == 96
        for name, value in [("VRAM_KERNEL_SIZE_MIB", str(limit + 1)), ("VRAM_KERNEL_SIZE_MIB", "-1"),
                            ("VRAM_OS_MAX_MIB", "0"), ("VRAM_PLATFORM_MAX_MIB", "oops")]:
            try:
                size(ram, **{name: value})
            except ValueError:
                continue
            raise AssertionError(f"accepted invalid {name}={value}")
print("Packaged native sizing, SMBIOS, coverage, header rounding, device cap and fallback verified.")
