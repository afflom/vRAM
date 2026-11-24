"""Exercise the installed launcher while a Linux guest runs ordinary memory accesses."""
import json
import mmap
import struct
import os
from pathlib import Path
import subprocess


def exercise(cli, state, qemu, bios, kernel, initrd, accelerator):
    command = [cli, "run", "--qemu", qemu, "--backing-dir", state.parent, "--state", state,
               "--ram-mib", "max", "--platform-max-mib", "512", "--guest-max-mib", "320",
               "--add-mib", "128", "--projection", "--", "-accel", accelerator, *(["-L", bios] if bios else []),
               "-kernel", kernel, "-initrd", initrd, "-append",
               "console=ttyS0 quiet nokaslr nmi_watchdog=0 rdinit=/init memhp_default_state=online panic=-1",
               "-nodefaults", "-display", "none", "-serial", "stdio", "-no-reboot"]
    def grow():
        return json.loads(subprocess.check_output([cli, "grow", "--state", state]))

    with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True) as child:
        try:
            for line in child.stdout:
                print(line.rstrip(), flush=True)
                if line.startswith("READY "):
                    dimm = grow()
                    if grow() != dimm or dimm["size"] != 128 << 20:
                        raise ValueError("duplicate resize or wrong extent")
                    child.stdin.write(f'{dimm["address"]} {dimm["size"]}\n')
                    child.stdin.flush()
                if line.startswith("WRITTEN "):
                    _, _, page, at = line.split()
                    for name in ("boot", "dimm"):
                        print("Syncing", name, flush=True)
                        with (state / "ram" / name).open("r+b") as f:
                            os.fsync(f.fileno())
                    # Read the guest physical sample through FUSE, bypassing its mapped page cache.
                    page = int(page)
                    seed = page + 1 if page % 16 == 0 else 1
                    expected = bytearray(4096)
                    for i in range(1024):
                        seed ^= (seed << 13) & 0xffffffff
                        seed ^= seed >> 17
                        seed ^= (seed << 5) & 0xffffffff
                        struct.pack_into("<I", expected, i * 4, page if i == 1 else seed)
                    fd = os.open(state / "ram/dimm", os.O_RDONLY | os.O_DIRECT)
                    try:
                        with mmap.mmap(-1, 4096) as buffer:
                            if os.preadv(fd, [buffer], int(at)) != 4096 or buffer[:] != expected:
                                raise ValueError("direct Verdant read differs from guest DIMM contents")
                    finally:
                        os.close(fd)
                    print("Verified the backing page", flush=True)
                    child.stdin.write("G\n")
                    child.stdin.flush()
            if child.wait() != 0 or state.exists():
                raise RuntimeError("guest exit or mount cleanup")
        finally:
            if child.poll() is None:
                child.terminate()


if __name__ == "__main__":
    import sys
    if len(sys.argv) > 1:
        cli, state, qemu, bios, kernel, initrd, accel = sys.argv[1:]
        exercise(cli, Path(state), qemu, bios, kernel, initrd, accel)
    else:
        exercise("/usr/bin/vram-machine", Path("/tmp/vm"), "/qemu", "/firmware", "/vmlinuz",
                 "/guest.gz", "kvm" if Path("/dev/kvm").exists() else "tcg")
