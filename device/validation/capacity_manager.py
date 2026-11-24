"""Fixed-cgroup capacity trial, inside the outer Linux host."""
import json
import socket
import subprocess
import time
from pathlib import Path
from manage import qmp

MIB = 1 << 20


def run_case(config, backend, group, rows):
    profile, hot, maximum = config["profile"], config["hot_mib"], config["maximum_mib"]
    monitor, mpeer = socket.socketpair()
    serial, speer = socket.socketpair()
    mf, sf = mpeer.fileno(), speer.fileno()
    ram_mib = config.get("guest_ram_mib", 132)
    mem = (f"memory-backend-verdant-tensor,id=ram,size={ram_mib}M,x-quiescent-reclaim=on"
           if backend != "ordinary" else f"memory-backend-ram,id=ram,size={ram_mib}M")
    if config.get("no_reserve"):
        mem += ",reserve=off"
    search_nodes = config.get("search_nodes", 49)
    if backend == "vram":
        mem += f",x-search-nodes={search_nodes}"
        mem += ",x-adaptive-search=" + ("on" if config.get("adaptive",True) else "off")
    target = config.get("resident_mib", 0)
    if backend == "vram" and target:
        mem += f",x-resident-target={target}M"
    binary = "/prior-qemu" if backend == "previous-vram" else config.get("qemu", "/qemu")
    accel = config.get("accel", "tcg")
    argv = [binary, "-S", "-accel", accel, "-machine", "pc,memory-backend=ram,mem-merge=off",
            "-m", str(ram_mib), "-smp", "1", "-object", mem, "-nodefaults", "-display", "none",
            "-monitor", "none", "-chardev", f"socket,id=m,fd={mf}", "-mon", "chardev=m,mode=control",
            "-chardev", f"socket,id=s,fd={sf}", "-serial", "chardev:s", "-L", config.get("bios_dir", "/firmware"),
            "-bios", config.get("rom", "/capacity.rom"), "-device", "isa-debug-exit,iobase=0xf4,iosize=4", "-no-reboot"]

    def enter_group():
        (group / "cgroup.procs").write_text("0")

    with subprocess.Popen(argv, pass_fds=(mf, sf), preexec_fn=enter_group) as vm:
        mpeer.close()
        speer.close()
        attempted = 0
        try:
            monitor.settimeout(120)
            serial.settimeout(120)
            with monitor, serial, monitor.makefile("rwb", buffering=0) as stream:
                assert "QMP" in json.loads(stream.readline())
                qmp(stream, "qmp_capabilities")
                if accel == "kvm":
                    assert qmp(stream, "query-kvm")["enabled"]
                qmp(stream, "cont")
                if serial.recv(1) != b"B":
                    raise EOFError("guest boot")
                serial.sendall(profile.encode())

                def guest(op, tile):
                    serial.sendall(op.encode() + tile.to_bytes(2 if maximum > 128 else 1, "little"))
                    ack = serial.recv(1)
                    if not ack:
                        raise EOFError("guest serial")
                    if ack != b"D":
                        raise RuntimeError(f"guest data mismatch at tile {tile} during {op}")

                def drop(tile):
                    # The controller owns a quiescent, DMA-free range in this firmware VM.
                    qmp(stream, "stop")
                    if backend != "ordinary":
                        info = qmp(stream, "x-vram-reclaim", {"id":"ram", "offset":(tile+2)*MIB, "length":MIB})
                        assert info["discarded-pages"] == 256 and info["resident-after"] == 0, info
                    qmp(stream, "cont")

                for size in range(1, maximum + 1):
                    attempted = size
                    guest("W", size-1)
                    if not target and size > hot:
                        drop(size-hot-1)
                    # Revisit resident pages at the same cadence in both arms.
                    for tile in range(max(0, size-hot), size):
                        guest("H", tile)
                    checkpoint = size in (1, 4, maximum) or size % 8 == 0
                    if config.get("geometric") and size >= 8:
                        checkpoint = (size & (size-1)) == 0 or size == maximum
                    if not checkpoint:
                        continue
                    start = time.monotonic()
                    for tile in range(size):
                        guest("V", tile)
                        if not target and tile < size-hot:
                            drop(tile)
                    seconds = time.monotonic() - start
                    qmp(stream, "stop")
                    row = dict(backend=backend, verified_mib=size, hot_mib=hot, profile=profile,
                               verify_and_reclaim_seconds=seconds,
                               group_current=int((group/"memory.current").read_text()),
                               group_peak=int((group/"memory.peak").read_text()))
                    stat = dict(line.split() for line in (group/"memory.stat").read_text().splitlines())
                    row["charge_bytes"] = {k: int(stat[k]) for k in ("anon","file","kernel","pagetables","shmem")}
                    if backend == "vram":
                        row["store"] = qmp(stream, "x-query-vram-store", {"id":"ram"})
                        if accel == "kvm":
                            assert not row["store"]["write-tracking"], row
                            assert row["store"]["clean-drops"] == 0, row
                        cold_pages = max(0, size-hot) * 256
                        if target and size >= 2 * target:
                            assert row["store"]["automatic-drops"] > 0, row
                            if profile == "S":
                                assert row["store"]["selector-restores"] > 0, row
                        if profile == "F":
                            if not search_nodes:
                                assert row["store"]["program-pages"] == 0, row
                            elif search_nodes == 49 and size >= max(8, 2 * target):
                                assert row["store"]["program-pages"] >= 256, row
                        if profile != "F" and not target and cold_pages:
                            expected_models = {"S": cold_pages, "U": 0, "M": cold_pages//2}[profile]
                            assert row["store"]["selector-pages"] == expected_models, row
                            assert row["store"]["selector-restores"] >= expected_models, row
                            assert row["store"]["raw-pages"] == cold_pages - expected_models, row
                    rows.append(row)
                    print("POINT", json.dumps(row), flush=True)
                    qmp(stream, "cont")
                serial.sendall(b"Q")
                assert vm.wait(timeout=30) == 1
                return "sweep ceiling", attempted
        except (EOFError, BrokenPipeError, ConnectionResetError, json.JSONDecodeError):
            code = vm.wait(timeout=30)
            events = dict(s.split() for s in (group/"memory.events").read_text().splitlines())
            if code != -9 or int(events["oom_kill"]) < 1:
                raise
            return "cgroup OOM kill", attempted
        finally:
            if vm.poll() is None:
                vm.kill()


def main():
    cfg = json.loads(Path("/capacity.json").read_text())
    root = Path("/sys/fs/cgroup")
    (root/"cgroup.subtree_control").write_text("+memory")
    for backend in cfg["order"]:
        group = root/backend
        group.mkdir()
        (group/"memory.max").write_text(str(cfg["budget_mib"]*MIB))
        (group/"memory.swap.max").write_text("0")
        rows = []
        outcome, attempted = run_case(cfg, backend, group, rows)
        events = dict(s.split() for s in (group/"memory.events").read_text().splitlines())
        print("END", json.dumps(dict(backend=backend, outcome=outcome, attempted_mib=attempted,
              last_fully_verified_mib=rows[-1]["verified_mib"] if rows else 0,
              peak_bytes=int((group/"memory.peak").read_text()), events=events)), flush=True)
        group.rmdir()
    print("PASS capacity sweep", flush=True)


if __name__ == "__main__":
    main()
