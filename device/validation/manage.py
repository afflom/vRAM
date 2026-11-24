"""Run inside the outer Linux environment and control the inner VM over QMP."""
import json
from pathlib import Path
import socket
import subprocess


def qmp(stream, command, args=None, expect_error=None):
    request = {"execute": command, "id": 1}
    if args is not None:
        request["arguments"] = args
    stream.write(json.dumps(request).encode() + b"\n")
    while True:
        line = stream.readline()
        if not line:
            raise EOFError(command)
        reply = json.loads(line)
        if "event" in reply:
            continue
        assert reply["id"] == 1, reply
        if expect_error:
            assert expect_error in reply["error"]["desc"], reply
            return
        return reply["return"]


def footprint(pid):
    fields = {}
    with open(f"/proc/{pid}/smaps_rollup") as summary:
        for line in summary:
            key, _, value = line.partition(":")
            if key in ("Rss", "Pss", "Swap"):
                fields[key.lower() + "_kib"] = int(value.split()[0])
    assert len(fields) == 3, fields
    return fields


CASES = [(2, "fill with boundary writes"), (256, "palette with boundary writes"),
         (1024, "fill with boundary writes"), (1024, "palette with boundary writes"),
         (1024, "pseudorandom with boundary writes"), (1024, "patch restored pseudorandom pages")]


def trial(backend, qemu="/qemu", bios_dir="/firmware", kernel="/vmlinuz", initrd="/guest.cpio.gz",
          budget_group=None):
    monitor, mon_peer = socket.socketpair()
    serial, ser_peer = socket.socketpair()
    mon_fd, ser_fd = mon_peer.fileno(), ser_peer.fileno()
    mem = ("memory-backend-verdant-tensor,id=ram,size=192M,x-quiescent-reclaim=on"
           if backend == "vram" else "memory-backend-ram,id=ram,size=192M")
    args = [qemu, "-machine", "pc,memory-backend=ram,mem-merge=off", "-accel", "tcg",
            "-cpu", "max", "-m", "192", "-smp", "1", "-object", mem, "-S", "-nodefaults",
            "-display", "none", "-monitor", "none", "-chardev", f"socket,id=m,fd={mon_fd}",
            "-mon", "chardev=m,mode=control", "-chardev", f"socket,id=s,fd={ser_fd}",
            "-serial", "chardev:s", "-device", "isa-debug-exit,iobase=0xf4,iosize=4",
            "-L", bios_dir, "-kernel", kernel, "-initrd", initrd,
            "-append", "console=ttyS0 earlyprintk=serial,ttyS0,115200 nokaslr rdinit=/init panic=-1", "-no-reboot"]
    def enter_budget_group():
        # Move the child before exec, before QEMU allocates guest RAM.
        (budget_group / "cgroup.procs").write_text("0")

    with subprocess.Popen(args, pass_fds=(mon_fd, ser_fd),
                          preexec_fn=enter_budget_group if budget_group else None) as vm:
        mon_peer.close()
        ser_peer.close()
        try:
            monitor.settimeout(120)
            serial.settimeout(300)
            with monitor, serial, monitor.makefile("rwb", buffering=0) as m, serial.makefile("rb") as guest:
                assert "QMP" in json.loads(m.readline())
                qmp(m, "qmp_capabilities")
                qmp(m, "cont")
                print("BOOT", backend, flush=True)
                cases = list(CASES)
                if Path("/linux-selector").exists():
                    cases.extend([(4096, "selector pages"), (4096, "one byte changed per 17 selector pages")])
                for phase, (pages, pattern) in enumerate(cases):
                    ranges = []
                    while True:
                        raw = guest.readline()
                        if not raw:
                            raise EOFError("guest serial closed")
                        line = raw.decode().strip()
                        if not line:
                            continue
                        if line.startswith("R "):
                            _, addr, size = line.split()
                            ranges.append((int(addr), int(size)))
                        elif line == f"READY {phase} {pages}":
                            break
                        elif line.startswith("FAIL"):
                            raise RuntimeError(f"guest handshake {line!r}")
                        elif any(word in line for word in ("Linux version", "Run /init", "panic", "BUG:", "error", "unavailable")):
                            print("guest", line, flush=True)
                    targets = [p for a, n in ranges for p in range(a, a+n, 4096)]
                    assert len(targets) == len(set(targets)) == pages
                    assert all(0x100000 <= a and n > 0 and a+n <= 192<<20
                               and a%4096 == n%4096 == 0 for a, n in ranges)

                    # Merge adjacent owned PFNs even if their virtual order differs.
                    ranges = []
                    for addr in sorted(targets):
                        if ranges and sum(ranges[-1]) == addr:
                            start, length = ranges[-1]
                            ranges[-1] = (start, length + 4096)
                        else:
                            ranges.append((addr, 4096))

                    def reclaim(addr, size, err=None, object_id="ram"):
                        return qmp(m, "x-vram-reclaim", {"id": object_id, "offset": addr,
                                                         "length": size}, err)

                    if phase == 0 and backend == "vram":
                        reclaim(*ranges[0], err="paused TCG")
                    qmp(m, "stop")
                    assert qmp(m, "query-status")["status"] == "paused"
                    if phase == 0:
                        if backend == "vram":
                            for addr, size in [(1, 4096), (0, 0), (192<<20, 4096), (2**64-4096, 8192)]:
                                reclaim(addr, size, "Invalid argument")
                            reclaim(*ranges[0], err="expected a vRAM backend", object_id="missing")
                        else:
                            reclaim(*ranges[0], err="expected a vRAM backend")
                    before_reclaim = footprint(vm.pid)
                    stored = None
                    if backend == "vram":
                        discarded = 0
                        for addr, size in ranges:
                            info = reclaim(addr, size)
                            assert info["resident-after"] == 0, info
                            discarded += info["discarded-pages"]
                        assert discarded == ((pages + 16)//17 if phase == 7 else pages), discarded
                        if phase >= 6:
                            stored = qmp(m, "x-query-vram-store", {"id":"ram"})
                            assert stored["selector-pages"] == pages, stored
                            assert stored["corrected-pages"] == stored["edit-count"] == (discarded if phase == 7 else 0), stored
                        before = info["total-faults"]
                        for addr, size in ranges:
                            info = reclaim(addr, size)
                            assert info == {"discarded-pages": 0, "resident-after": 0, "total-faults": before}, info
                    after_reclaim = footprint(vm.pid)
                    qmp(m, "cont")
                    serial.sendall(b"G")
                    assert guest.readline().decode().strip() == f"VERIFIED {phase} {pages*4096}"
                    qmp(m, "stop")
                    row = dict(before_reclaim=before_reclaim, after_reclaim=after_reclaim,
                               after_readback=footprint(vm.pid), backend=backend, phase=phase, checked_bytes=pages*4096,
                               physical_ranges=len(ranges), pages=pages, pattern=pattern)
                    if budget_group:
                        row["cgroup_peak_bytes_to_readback"] = int((budget_group / "memory.peak").read_text())
                    if backend == "vram":
                        for addr, size in ranges:
                            info = reclaim(addr, size)
                            assert info["discarded-pages"] == size//4096 and info["resident-after"] == 0, info
                        if stored is not None:
                            row["backing_before_readback"] = stored
                            row["backing_after_readback"] = qmp(m, "x-query-vram-store", {"id":"ram"})
                            after = row["backing_after_readback"]
                            assert after["selector-restores"] - stored["selector-restores"] == pages, after
                            if stored["write-tracking"]:
                                assert after["clean-drops"] - stored["clean-drops"] == pages, after
                                assert after["captures"] == stored["captures"], after
                        delta = info["total-faults"] - before
                        assert delta >= pages, (delta, pages)
                        row.update(discarded_before_resume=discarded, resident_after_discard=0,
                                   discarded_on_retry=0, discarded_after_readback=pages,
                                   backend_faults_during_readback=delta)
                    qmp(m, "cont")
                    print("RESULT", json.dumps(row), flush=True)
                    serial.sendall(b"G")
                assert guest.readline().decode().strip() == "GUEST PASS"
                assert vm.wait(timeout=30) == 1
        finally:
            if vm.poll() is None:
                vm.kill()


if __name__ == "__main__":
    cgroup = Path("/sys/fs/cgroup")
    (cgroup / "cgroup.subtree_control").write_text("+memory")
    for backend in ("ordinary", "vram"):
        group = cgroup / backend
        group.mkdir()
        try:
            (group / "memory.max").write_text(str(256 << 20))
            (group / "memory.swap.max").write_text("0")
            trial(backend, budget_group=group)
            events = dict(line.split() for line in (group / "memory.events").read_text().splitlines())
            print("BUDGET", json.dumps({"backend": backend, "limit_bytes": 256 << 20,
                  "peak_bytes": int((group / "memory.peak").read_text()),
                  "swap_limit_bytes": 0, "memory_events": {k: int(v) for k, v in events.items()}}),
                  flush=True)
        finally:
            group.rmdir()
    print("PASS Linux guest dirty-page reclaim", flush=True)
