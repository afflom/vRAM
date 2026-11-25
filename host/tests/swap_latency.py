"""Compare queue-depth-one 4 KiB block I/O on fresh zram and vRAM devices in a disposable VM."""
import argparse
import gzip
import hashlib
import json
import lzma
from pathlib import Path
import re
import stat
import statistics
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "device/validation"))
from run import archive

parser = argparse.ArgumentParser(description=__doc__)
for name in ("qemu", "kernel", "modules", "result"):
    parser.add_argument("--" + name, type=Path, required=True)
parser.add_argument("--control", type=Path, help="NBD control server for comparisons using NBD")
parser.add_argument("--candidate", type=Path, default=root / "host/build/vram-host")
parser.add_argument("--bios-dir", type=Path)
parser.add_argument("--accel", choices=["kvm", "tcg"], required=True)
parser.add_argument("--require-parity", action="store_true", help="exit unsuccessfully if any median exceeds zram")
parser.add_argument("--trace-output", type=Path, help="write matched timestamps from the diagnostic control server as gzip JSON")
parser.add_argument("--local-completion", action="store_true",
                    help="compare rq_affinity=0 with the guest default on two copies of the candidate")
parser.add_argument("--read-windows", action="store_true", help="compare byte-verified direct reads at six request sizes")
parser.add_argument("--kernel-literal", type=Path, help="measure the resident literal module alongside the shipping server and zram")
parser.add_argument("--kernel-fill", type=Path, help="compare the resident Fill store with the literal kernel control and zram")
parser.add_argument("--kernel-system", type=Path, help="installed DKMS activation helper and userland for the Fill guest")
args = parser.parse_args()
if bool(args.kernel_fill) != bool(args.kernel_system):
    parser.error("the Fill guest requires the installed kernel activation root")
if args.kernel_fill and not args.kernel_literal:
    parser.error("the Fill comparison requires the kernel literal control")
if args.kernel_literal and (args.local_completion or args.read_windows or args.trace_output):
    parser.error("the literal comparison requires the ordinary single-page workload")
if args.read_windows and (args.local_completion or args.trace_output):
    parser.error("read window measurements use the uninstrumented release comparison")
if not args.kernel_fill and not args.control:
    parser.error("NBD comparisons require --control")
servers = {} if args.kernel_fill else {key: getattr(args, key).read_bytes() for key in ("control", "candidate")}
if args.kernel_literal and not args.kernel_fill and servers["control"] != servers["candidate"]:
    parser.error("the literal comparison requires the shipping candidate as control")
if args.local_completion and (servers["control"] != servers["candidate"] or args.trace_output):
    parser.error("completion placement requires identical uninstrumented servers")

def python_files():
    # Collect the system interpreter, its imported standard library and resolved ELF dependencies.
    python = "/usr/bin/python3"
    probe = ("import os,sys,statistics,random,mmap,time,json;"
             "print(json.dumps([m.__file__ for m in sys.modules.copy().values() if getattr(m,'__file__',None)]))")
    paths = {Path(p) for p in json.loads(subprocess.check_output([python, "-IS", "-c", probe]))}
    paths.add(Path(python))
    for path in list(paths):
        if path == Path(python) or path.suffix == ".so":
            linked = subprocess.check_output(["ldd", path], text=True)
            paths.update(Path(p) for p in re.findall(r"(/[^\s]+)\s+\(", linked))
    return {str(path).lstrip("/"): path.read_bytes() for path in paths}


with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    subprocess.run(["cc", "-static", "-O2", "-Wall", "-Wextra", "-Werror",
                    root / "host/tests/swap_latency_guest.c", "-o", path / "init"], check=True)
    files = dict(servers, init=(path / "init").read_bytes())
    if args.kernel_literal:
        files.pop("candidate", None)
        files["kernel-literal.ko"] = args.kernel_literal.read_bytes()
    if args.kernel_fill:
        subprocess.run(["cc", "-static", "-O2", "-Wall", "-Wextra", "-Werror", root / "host/tests/kernel_reclaim.c",
                        "-o", path / "kernel-reclaim"], check=True)
        files["kernel-reclaim"] = (path / "kernel-reclaim").read_bytes()
        files["kernel-segments.ko"] = args.kernel_literal.with_name("vram_segments.ko").read_bytes()
        installed, = args.kernel_system.glob("lib/modules/*/updates/vram_fill.ko")
        if installed.read_bytes() != args.kernel_fill.read_bytes():
            parser.error("the timed module differs from the installed activation module")
        for item in args.kernel_system.rglob("*"):
            if item.is_file():
                files["kernel-system/" + str(item.relative_to(args.kernel_system))] = item.read_bytes()
        files["kernel-fill.ko"] = args.kernel_fill.read_bytes()
        files["swap_fill.py"] = (root / "host/tests/swap_fill.py").read_bytes()
    files.update(python_files())
    files["workload.py"] = (root / "host/tests/swap_latency_workload.py").read_bytes()
    if args.read_windows:
        files["swap_read_windows.py"] = (root / "host/tests/swap_read_windows.py").read_bytes()
    if args.local_completion:
        files["local-completion"] = b""
    if args.trace_output:
        files["store-clock"] = bytes(5 * 8)
        files["swap_clock.py"] = (root / "host/tests/swap_clock.py").read_bytes()
    modules = ["zram", "lz4_compress", "lz4hc_compress"]
    if not args.kernel_fill:
        modules.append("nbd")
    for name in modules:
        module, = args.modules.rglob(name + ".ko.xz")
        files[name + ".ko"] = lzma.decompress(module.read_bytes())
    entries = {d: (stat.S_IFDIR | 0o755, b"", 0, 0) for d in ("dev", "sys", "proc")}
    for f in files:
        for parent in reversed(Path(f).parents):
            if str(parent) != ".":
                entries[str(parent)] = (stat.S_IFDIR | 0o755, b"", 0, 0)
    entries["dev/urandom"] = (stat.S_IFCHR | 0o666, b"", 1, 9)
    entries["dev/console"] = (stat.S_IFCHR | 0o600, b"", 5, 1)
    entries.update({k: (stat.S_IFREG | 0o755, v, 0, 0) for k, v in files.items()})
    entries["TRAILER!!!"] = (0, b"", 0, 0)
    (path / "initrd").write_bytes(archive(entries))
    command = [args.qemu, "-accel", args.accel, "-smp", "2", "-m", "384", "-kernel", args.kernel,
               "-initrd", path / "initrd", "-nodefaults", "-display", "none", "-serial", "stdio",
               "-device", "isa-debug-exit,iobase=0xf4,iosize=4", "-no-reboot",
               "-append", "console=ttyS0 quiet zswap.enabled=0 rdinit=/init panic=-1"]
    if args.trace_output:
        trace_file = path / "store-trace.json"
        command.extend(["-chardev", f"file,id=clock,path={trace_file}",
                        "-device", "isa-debugcon,iobase=0x505,chardev=clock"])
    if args.bios_dir:
        command.extend(["-L", args.bios_dir])
    if args.accel == "kvm":
        command.extend(["-cpu", "host"])
    def boot(argv):
        proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=360)
        output = proc.stdout.decode(errors="replace")
        if proc.returncode != 1:
            sys.exit(output[-4000:])
        return output

    log = boot(command)
    if args.kernel_fill:
        # Use a separate boot so directory allocation cannot affect the latency comparison.
        large = command.copy()
        large[large.index("-m") + 1] = "6144"
        large[large.index("-append") + 1] += " -- geometry"
        geometry_rows = re.findall(r"^KERNEL_GEOMETRY (.+)$", boot(large), re.M)
        if len(geometry_rows) != 1:
            sys.exit("Missing sparse device geometry evidence")
        geometry = json.loads(geometry_rows[0])
    if args.trace_output:
        batches = json.loads(trace_file.read_bytes())

candidate = "kernel-literal" if args.kernel_literal else "vram"
control = "vram-nbd" if args.kernel_literal else "control"
ratio = "literal_to_zram" if args.kernel_literal else "vram_to_zram"

if args.kernel_fill:
    candidate, control, ratio = "kernel-fill", "kernel-literal", "fill_to_zram"

def observations(prefix, profiles, operations=("write", "read")):
    rows = {}
    for line in re.findall(r"^" + prefix + r" (.+)$", log, re.M):
        run, backend, profile, op, p50, p99 = json.loads(line)
        key = (run, backend, profile, op)
        if key in rows or not 0 < p50 <= p99:
            sys.exit("Invalid timing observation: " + repr(key))
        rows[key] = dict(p50_ns=p50, p99_ns=p99)
    if len(rows) != len(profiles) * len(operations) * 9:
        sys.exit("Incomplete timing matrix\n" + log[-4000:])
    summary = {}
    for profile in profiles:
        for op in operations:
            times = {b: [rows[i,b,profile,op] for i in range(3)] for b in ("zram-zstd", control, candidate)}
            medians = {b: statistics.median(r["p50_ns"] for r in samples) for b, samples in times.items()}
            summary[f"{profile}_{op}"] = {"runs": times, "median_ns": medians,
                ratio: medians[candidate]/medians["zram-zstd"], "meets_zram": medians[candidate] <= medians["zram-zstd"]}
    return summary


summary = observations("PAGE_LATENCY", ("patterned", "dense"))
mixed = observations("MIXED_LATENCY", ("sparse", "selector", "period97", "perturbed", "independent", "fill"))
result = dict(accelerator=args.accel, kernel=args.kernel.name, kernel_sha256=hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
              server_sha256={k: hashlib.sha256(v).hexdigest() for k,v in servers.items()},
              python_version=re.search(r"^INTERPRETER ([0-9.]+)", log, re.M)[1], samples_per_run=2048, page_bytes=4096, queue_depth=1, runs=3,
              verified_bytes_per_backend=3*2*2048*4096,
              operator_parity=all(v["meets_zram"] for v in summary.values()),
              held_out_parity=all(v["meets_zram"] for v in mixed.values()),
              workloads=summary, mixed_overwrites=mixed, mixed_pages_per_family_per_round=64,
              mixed_verified_bytes_per_backend=3*384*4096)
if args.kernel_literal:
    addresses = re.findall(r"^LITERAL_POLICY (blake3:[0-9a-f]{64})\s*$", log, re.M)
    if len(addresses) != 1:
        sys.exit("Missing kernel literal policy identity")
    result["literal_policy"] = addresses[0]
    result["literal_payload_bytes"] = 64 << 20
    if not args.kernel_fill:
        result["server_sha256"] = {control: hashlib.sha256(servers["control"]).hexdigest(),
                                   candidate: hashlib.sha256(files["kernel-literal.ko"]).hexdigest()}
if args.kernel_fill:
    if re.findall(r"^FILL_POLICY (blake3:[0-9a-f]{64})\s*$", log, re.M) != addresses:
        sys.exit("Fill policy identity mismatch")
    pressure = re.findall(r"^KERNEL_RECLAIM (.+)$", log, re.M)
    if len(pressure) != 1:
        sys.exit("Missing kernel reclaim evidence")
    result["kernel_reclaim"] = json.loads(pressure[0])
    native = re.findall(r"^KERNEL_NATIVE (.+)$", log, re.M)
    if len(native) != 1:
        sys.exit("Missing automatic sizing evidence")
    result["native_capacity"] = json.loads(native[0])
    result["kernel_geometry"] = geometry
    result["range_throughput"] = [json.loads(row) for row in re.findall(r"^KERNEL_RANGE (.+)$", log, re.M)]
    result["fill_store"] = [json.loads(row) for row in re.findall(r"^FILL_STORE (.+)$", log, re.M)]
    result["server_sha256"] = {name: hashlib.sha256(files[name + ".ko"]).hexdigest() for name in (control, candidate)}
if args.read_windows:
    sizes = [1, 2, 16, 64, 65, 128]
    result["read_windows"] = observations("WINDOW_LATENCY", [f"pages{n}" for n in sizes], ("read",))
    result["read_window_samples_per_run"] = 128
    result["read_window_verified_bytes_per_backend"] = 3 * 128 * 4096 * sum(sizes)
if args.local_completion:
    settings = re.findall(r"^COMPLETION_AFFINITY ([0-2]) ([0-2])\s*$", log, re.M)
    if len(settings) != 1 or settings[0][1] != "0":
        sys.exit("Missing completion affinity readback")
    result["rq_affinity"] = dict(zip(("control", "vram"), map(int, settings[0])))
result["meets_parity"] = result["operator_parity"] and result["held_out_parity"]
if args.trace_output:
    if not batches:
        sys.exit("Missing store timing samples")
    payload = gzip.compress(json.dumps(batches, separators=(",", ":")).encode(), mtime=0)
    args.trace_output.write_bytes(payload)
    result["trace_sha256"] = hashlib.sha256(payload).hexdigest()
args.result.write_text(json.dumps(result, indent=2) + "\n")
print({k:v["median_ns"] for k,v in summary.items()})
if args.require_parity and not result["meets_parity"]:
    misses = [f"{suite}/{k}" for suite, cells in (("operator", summary), ("mixed", mixed))
              for k, v in cells.items() if not v["meets_zram"]]
    sys.exit("Single-page zram parity not reached: " + ", ".join(misses))
