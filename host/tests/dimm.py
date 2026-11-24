"""Boot Linux on stock QEMU memory-backend-file, hot-add a DIMM and verify its bytes."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import stat
import os
import subprocess
import sys
import tempfile
import threading

here = Path(__file__).resolve().parent
validation = here.parents[1] / "device/validation"
sys.path.insert(0, str(validation))
from reclaim import add, add_linked, add_python_firmware, base_entries, save
from run import svm_entries

parser = argparse.ArgumentParser(description=__doc__)
for name in ("qemu", "kernel", "package", "result"):
    parser.add_argument("--" + name, type=Path, required=True)
parser.add_argument("--bios-dir", type=Path)
parser.add_argument("--svm-modules", type=Path)
parser.add_argument("--native", action="store_true", help="run FUSE on the local Linux host using its filesystem")
args = parser.parse_args()
if not args.native and not args.bios_dir:
    parser.error("--bios-dir is required for the nested test")
with tempfile.TemporaryDirectory() as folder:
    tmp = Path(folder)
    payload = args.package.read_bytes()
    digest = hashlib.sha256(payload).hexdigest()
    staged = tmp / "package.deb"
    staged.write_bytes(payload)
    subprocess.run(["dpkg-deb", "-x", staged, tmp/"pkg"], check=True)
    cflags = ["cc", "-O2", "-static", "-Wall", "-Wextra", "-Werror"]
    subprocess.run([*cflags, "-I", tmp/"pkg/usr/include", here/"dimm_guest.c", here/"projection_guest.c", "-o", tmp/"init"], check=True)
    guest = base_entries()
    add(guest, "init", tmp/"init", 0o755)
    save(guest, tmp/"guest.gz")
    if args.native:
        cmd = [sys.executable, here/"dimm_manager.py", tmp/"pkg/usr/bin/vram-machine", tmp/"vm",
               args.qemu, args.bios_dir or "", args.kernel, tmp/"guest.gz", "kvm" if os.access("/dev/kvm", os.R_OK | os.W_OK) else "tcg"]
    else:
        outer = base_entries()
        subprocess.run([*cflags, "-DTEST_DIMM", *(["-DTEST_SVM"] if args.svm_modules else []),
                        validation/"reclaim_host.c", "-o", tmp/"host"], check=True)
        if args.svm_modules:
            svm_entries(outer, args.svm_modules)
        add(outer, "init", tmp/"host", 0o755)
        add(outer, "manage.py", here/"dimm_manager.py")
        add(outer, "vmlinuz", args.kernel)
        add(outer, "guest.gz", tmp/"guest.gz")
        add(outer, "usr/bin/vram-machine", tmp/"pkg/usr/bin/vram-machine", 0o755)
        add_linked(outer, tmp/"pkg/usr/lib/vram/vram-ram-file", "usr/lib/vram/vram-ram-file")
        outer["dev/fuse"] = (stat.S_IFCHR | 0o666, b"", 10, 229)
        add_linked(outer, Path("/usr/bin/fusermount3"), "usr/bin/fusermount3")
        subprocess.run(["strip", "-o", tmp/"qemu", args.qemu], check=True)
        add_linked(outer, tmp/"qemu", "qemu")
        # Libraries from an extracted distribution package need a loader search path inside the VM.
        for name in list(outer):
            if name.endswith(".so") or ".so." in name:
                if not name.startswith(("lib", "usr/lib")):
                    outer["lib/x86_64-linux-gnu/" + Path(name).name] = outer[name]
        add_python_firmware(outer, Path("/usr/bin/python3"), args.bios_dir)
        save(outer, tmp/"outer.gz")
        cmd = [args.qemu, "-L", args.bios_dir, "-accel", "tcg", "-smp", "2", "-m", "2560",
               "-cpu", "max,vendor=AuthenticAMD,+svm" if args.svm_modules else "max",
               "-kernel", args.kernel, "-initrd", tmp/"outer.gz", "-append",
               "console=ttyS0 quiet nokaslr rdinit=/init panic=-1", "-nodefaults", "-display", "none",
               "-serial", "stdio", "-no-reboot", "-device", "isa-debug-exit,iobase=0xf4,iosize=4"]
    print("Booting packaged vRAM and a Linux guest", flush=True)
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace") as proc:
        timer = threading.Timer(1200, proc.kill)
        timer.start()
        lines = []
        try:
            for line in proc.stdout:
                print(line.rstrip(), flush=True)
                lines.append(line)
            proc.wait()
        finally:
            timer.cancel()
        log = "".join(lines)
    def row(name):
        matches = re.findall(r"^" + name + r" (\{[^\r\n]+\})", log, re.M)
        if len(matches) != 1:
            raise ValueError(f"expected one {name} receipt")
        return json.loads(matches[0])
    guest, store = row("DIMM_RESULT"), row("VRAM_FILE")
    if ("Kernel panic" in log or "Verified the backing page" not in log
            or proc.returncode != (0 if args.native else 1) or store["write_pages"] < 256 or store["read_pages"] < 256
            or not store["file_base_writes"] or not store["file_base_reads"]
            or guest["verified_on_dimm_bytes"] < 1 << 20):
        raise RuntimeError("missing Linux or Verdant memory traffic")
    result = dict(guest=guest, store=store, dimm_mib=128, initial_mib=192,
                  sizing="max", platform_max_mib=512, guest_max_mib=320,
                  acceleration="KVM on emulated AMD SVM" if args.svm_modules else (cmd[-1].upper() if args.native else "nested TCG"),
                  qemu=subprocess.check_output([args.qemu, "--version"], text=True).splitlines()[0],
                  kernel=args.kernel.name, package_sha256=digest)
    trials = [json.loads(match) for match in re.findall(r"^PROJECTION_TRIAL (.+)$", log, re.M)]
    if (log.splitlines().count("PROJECTION_VERIFIED") != 1 or [r["trial"] for r in trials] != list(range(24))
            or any(min(r[k] for k in ("packed_ns", "restore_cpu_ns", "resident_cpu_ns")) <= 0 for r in trials)):
        raise RuntimeError("missing validated projection trials")
    result["projection"] = dict(port="org.verdant.project.1", columns=64, verified_trials=len(trials), trials=trials)
    args.result.write_text(json.dumps(result, indent=2) + "\n")
