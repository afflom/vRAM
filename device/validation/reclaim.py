"""Build two small Linux initramfs images and verify dirty main RAM under QEMU."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import stat
import subprocess
import tempfile
import threading

from run import archive, svm_entries


HERE = Path(__file__).resolve().parent


def base_entries():
    directories = ("dev", "proc", "sys", "tmp")
    entries = {d: (stat.S_IFDIR | 0o755, b"", 0, 0) for d in directories}
    for name, major, minor in [("console", 5, 1), ("ttyS0", 4, 64), ("null", 1, 3),
                               ("zero", 1, 5), ("urandom", 1, 9), ("random", 1, 8)]:
        entries["dev/" + name] = (stat.S_IFCHR | 0o666, b"", major, minor)
    return entries


def add(entries, name, path, mode=0o644):
    for parent in reversed(Path(name).parents):
        if str(parent) != ".":
            entries.setdefault(str(parent), (stat.S_IFDIR | 0o755, b"", 0, 0))
    entries[name] = (stat.S_IFREG | mode, path.read_bytes(), 0, 0)


def add_linked(entries, path, name):
    add(entries, name, path, 0o755)
    libs = subprocess.check_output(["ldd", path], text=True)
    for lib in re.findall(r"(/[^\s]+) \(0x", libs):
        add(entries, lib.lstrip("/"), Path(lib), 0o755)


def save(entries, path):
    entries["TRAILER!!!"] = (0, b"", 0, 0)
    path.write_bytes(archive(entries))


def add_python_firmware(outer, system_python, bios_dir):
    add_linked(outer, system_python, "usr/bin/python3")
    probe = ("import argparse, mmap, struct, resource, signal, tempfile, sys, sysconfig, json, socket, subprocess, codecs, pathlib, encodings.utf_8; "
             "print(json.dumps([sysconfig.get_path('stdlib'),"
             "[m.__file__ for m in sys.modules.values() if "
             "str(getattr(m,'__file__','')).endswith('.so')]]))")
    stdlib, extensions = json.loads(subprocess.check_output([system_python, "-S", "-c", probe]))
    # Use a distro Python whose standard library lives under /usr.
    if not Path(stdlib).is_relative_to("/usr/lib"):
        raise ValueError("use a system Python installed under /usr with stdlib under /usr/lib")
    for file in Path(stdlib).rglob("*.py"):
        if not {"site-packages", "dist-packages", "test", "tests"}.intersection(file.parts):
            add(outer, str(file).lstrip("/"), file)
    outer[stdlib.lstrip("/") + "/lib-dynload"] = (stat.S_IFDIR | 0o755, b"", 0, 0)
    for lib in extensions:
        add_linked(outer, Path(lib), lib.lstrip("/"))
    for glob in ("*.bin", "*.rom"):
        for file in bios_dir.glob(glob):
            add(outer, "firmware/" + file.name, file)


def build_rom(tmp, maximum_mib):
    subprocess.run(["cc", "-m32", "-O2", "-ffreestanding", "-fno-pie", "-fno-stack-protector",
                    *([f"-DTILES={maximum_mib}"] if maximum_mib > 128 else []),
                    "-c", HERE/"capacity_guest.c", "-o", tmp/"guest.o"], check=True)
    subprocess.run(["as", "--32", HERE/"start.S", "-o", tmp/"start.o"], check=True)
    subprocess.run(["ld", "-m", "elf_i386", "-T", HERE/"rom.ld", tmp/"start.o", tmp/"guest.o",
                    "-o", tmp/"guest.elf"], check=True)
    subprocess.run(["objcopy", "-O", "binary", tmp/"guest.elf", tmp/"capacity.rom"], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("qemu", "kernel", "bios-dir", "result"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--system-python", type=Path, default=Path("/usr/bin/python3"))
    parser.add_argument("--linux-selector", action="store_true")
    parser.add_argument("--capacity-profile", choices=("S", "M", "U", "F"))
    parser.add_argument("--hot-mib", type=int, default=1)
    parser.add_argument("--maximum-mib", type=int, default=64)
    parser.add_argument("--budget-mib", type=int, default=32)
    parser.add_argument("--fixed-search", action="store_true", help="disable regional feedback")
    parser.add_argument("--search-nodes", type=int, default=49, choices=range(50), metavar="0..49")
    parser.add_argument("--resident-mib", type=int, default=0, help="autonomous resident target for vRAM; omits all QMP range hints")
    parser.add_argument("--prior-qemu", type=Path, help="also test a previous vRAM binary")
    parser.add_argument("--geometric", action="store_true", help="double verification steps above 8 MiB")
    parser.add_argument("--reverse", action="store_true")
    parser.add_argument("--svm-modules", type=Path, help="run inner KVM with the outer kernel's AMD SVM modules")
    args = parser.parse_args()
    if args.svm_modules and (not args.capacity_profile or args.prior_qemu):
        parser.error("KVM capacity testing requires a firmware profile and a single vRAM version")
    ram_mib = max(132, args.maximum_mib + 4)
    if args.resident_mib and (not args.capacity_profile or args.prior_qemu or not 1 <= args.resident_mib <= ram_mib):
        parser.error("an in-bounds automatic target requires a capacity profile and no prior binary")
    if args.linux_selector and args.capacity_profile:
        parser.error("select one guest workload")
    if not 1 <= args.hot_mib <= args.maximum_mib <= 1024 or args.budget_mib <= 0:
        parser.error("expected 1 <= hot <= maximum <= 1024 MiB and a positive host budget")
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        for component in ("host", "guest"):
            subprocess.run(["cc", "-static", "-Wall", "-Wextra", "-Werror", "-O2",
                            *(["-DTEST_SVM"] if component == "host" and args.svm_modules else []),
                            *(["-DSELECTOR_TEST"] if component == "guest" and args.linux_selector else []),
                            HERE / f"reclaim_{component}.c", "-o", tmp/component], check=True)
        guest = base_entries()
        add(guest, "init", tmp/"guest", 0o755)
        save(guest, tmp/"guest.cpio.gz")
        outer = base_entries()
        if args.svm_modules:
            svm_entries(outer, args.svm_modules)
        add(outer, "init", tmp/"host", 0o755)
        add(outer, "guest.cpio.gz", tmp/"guest.cpio.gz")
        add(outer, "vmlinuz", args.kernel)
        add(outer, "manage.py", HERE/"manage.py")
        if args.linux_selector:
            outer["linux-selector"] = (stat.S_IFREG | 0o444, b"", 0, 0)
        if args.capacity_profile:
            config = dict(profile=args.capacity_profile, hot_mib=args.hot_mib,
                          maximum_mib=args.maximum_mib, budget_mib=args.budget_mib, guest_ram_mib=ram_mib,
                          order=["vram","ordinary"] if args.reverse else ["ordinary","vram"])
            config["accel"] = "kvm" if args.svm_modules else "tcg"
            config["geometric"] = args.geometric
            config["resident_mib"] = args.resident_mib
            config["search_nodes"] = args.search_nodes
            config["adaptive"] = not args.fixed_search
            config["no_reserve"] = ram_mib >= 1024
            if args.prior_qemu:
                config["order"].insert(1, "previous-vram")
                config["prior_qemu_sha256"] = hashlib.sha256(args.prior_qemu.read_bytes()).hexdigest()
                add_linked(outer, args.prior_qemu, "prior-qemu")
            (tmp/"capacity.json").write_text(json.dumps(config))
            build_rom(tmp, args.maximum_mib)
            for name in ("capacity.rom", "capacity.json"):
                add(outer, name, tmp/name)
            add(outer, "capacity_manager.py", HERE/"capacity_manager.py")
        subprocess.run(["strip", "-o", tmp/"qemu", args.qemu], check=True)
        add_linked(outer, tmp/"qemu", "qemu")
        inner_hash = hashlib.sha256((tmp/"qemu").read_bytes()).hexdigest()
        add_python_firmware(outer, args.system_python, args.bios_dir)
        save(outer, tmp/"outer.cpio.gz")
        cmd = [args.qemu, "-accel", "tcg", "-smp", "2", "-m", "1024", "-machine", "pc",
               "-cpu", "max,vendor=AuthenticAMD,+svm" if args.svm_modules else "max", "-kernel", args.kernel, "-initrd", tmp/"outer.cpio.gz",
               "-append", "console=ttyS0 quiet nokaslr rdinit=/init panic=-1", "-L", args.bios_dir,
               "-nodefaults", "-display", "none", "-monitor", "none", "-serial", "stdio",
               "-device", "isa-debug-exit,iobase=0xf4,iosize=4", "-no-reboot"]
        with subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True) as vm:
            watchdog = threading.Timer(7200 if args.capacity_profile else 900, vm.kill)
            watchdog.start()
            rows = []
            budgets = []
            passed = False
            try:
                for line in vm.stdout:
                    print(line.rstrip(), flush=True)
                    if line.startswith("RESULT "):
                        rows.append(json.loads(line[7:]))
                    if line.startswith("BUDGET "):
                        budgets.append(json.loads(line[7:]))
                    if line.startswith("POINT "):
                        rows.append(json.loads(line[6:]))
                    if line.startswith("END "):
                        budgets.append(json.loads(line[4:]))
                    if line.strip() == ("PASS capacity sweep" if args.capacity_profile else "PASS Linux guest dirty-page reclaim"):
                        passed = True
            finally:
                watchdog.cancel()
                if not passed and vm.poll() is None:
                    vm.kill()
            if vm.wait() != 1 or not passed or (not args.capacity_profile and len(rows) != (16 if args.linux_selector else 12)) or len(budgets) != (3 if args.prior_qemu else 2):
                raise RuntimeError("nested Linux verification failed")
        args.result.write_text(json.dumps({
            "qemu": subprocess.check_output([args.qemu, "--version"], text=True).splitlines()[0],
            "kernel_sha256": hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
            "inner_qemu_sha256": inner_hash,
            "guest_image_sha256": hashlib.sha256((tmp/("capacity.rom" if args.capacity_profile else "guest.cpio.gz")).read_bytes()).hexdigest(),
            "acceleration": "Linux KVM on TCG-emulated AMD SVM" if args.svm_modules else "TCG inside TCG", "guest_ram_mib": ram_mib if args.capacity_profile else 192, "inner_vcpus": 1,
            **({"config": config} if args.capacity_profile else {}),
            "budgets": budgets, "trials": rows}, indent=2) + "\n")


if __name__ == "__main__":
    main()
