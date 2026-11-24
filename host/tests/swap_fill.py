"""Allocation and readback checks on the disposable kernel Fill device."""
import json
import mmap
import os
import random
import time

PAGE = 4096
SLOTS = 16384
PARAMS = "/sys/module/vram_fill/parameters/"


def parameter(name):
    with open(PARAMS + name) as source:
        return source.read().strip()


def transition(fd, buf):
    assert int(parameter("literal_pages")) == 0
    for slot in (0, SLOTS - 1):
        assert os.preadv(fd, [buf], slot * PAGE) == PAGE and buf[:] == bytes(PAGE)
    for i in (256, 257):
        offset = i * PAGE
        buf[:] = bytes([0x85]) * (PAGE - 1) + b"\x93"
        assert os.pwrite(fd, buf, offset) == PAGE
        assert int(parameter("literal_pages")) == 1
        buf[:] = bytes(PAGE)
        assert os.preadv(fd, [buf], offset) == PAGE and buf[-1] == 0x93 and buf[:-1] == bytes([0x85]) * (PAGE - 1)
        buf[:] = bytes([0x93]) * PAGE
        assert os.pwrite(fd, buf, offset) == PAGE
        assert int(parameter("literal_pages")) == 0
    # Both processes reuse the same slot, with independent aligned I/O buffers.
    offset = 256 * PAGE
    fills = bytes([0x93]) * PAGE
    dense = fills[:-1] + b"U"
    child = os.fork()
    if child == 0:
        try:
            with mmap.mmap(-1, PAGE) as writer:
                for i in range(1024):
                    writer[:] = fills if i % 2 else dense
                    assert os.pwrite(fd, writer, offset) == PAGE
        except BaseException:
            os._exit(1)
        os._exit(0)
    other = bytes([0x2a]) * (PAGE - 1) + b"W"
    for _ in range(1024):
        buf[:] = other
        assert os.pwrite(fd, buf, offset) == PAGE
        assert os.preadv(fd, [buf], offset) == PAGE and buf[:] in (other, fills, dense)
    assert os.waitpid(child, 0) == (child, 0)
    buf[:] = fills
    assert os.pwrite(fd, buf, offset) == PAGE and int(parameter("literal_pages")) == 0
    print("FILL_POLICY", parameter("policy"), flush=True)


def snapshot(stage, written, expected_literals, read_p99=None):
    literal = int(parameter("literal_pages"))
    directory = int(parameter("directory_bytes"))
    assert literal == expected_literals
    charged = directory + literal * PAGE
    print("FILL_STORE", json.dumps(dict(stage=stage, written_pages=written, literal_pages=literal,
          directory_bytes=directory, charged_bytes=charged, net_saved_bytes=written * PAGE - charged,
          read_p99_ns=read_p99)), flush=True)


def sweep(fd, buf):
    order = list(range(SLOTS))
    random.Random(2963).shuffle(order)
    # Every dense slot has distinct bytes.
    dense = bytes((i % 251 for i in range(PAGE - 4)))
    for stage, fills in (("all_literal", 0), ("half_fill", SLOTS // 2), ("all_fill", SLOTS)):
        def payload(slot):
            if slot < fills:
                return bytes([slot % 256]) * PAGE
            return slot.to_bytes(4, "little") + dense

        for slot in range(SLOTS):
            buf[:] = payload(slot)
            assert os.pwrite(fd, buf, slot * PAGE) == PAGE
        times = {"fill": [], "literal": []}
        for slot in order:
            offset = slot * PAGE
            start = time.perf_counter_ns()
            count = os.preadv(fd, [buf], offset)
            elapsed = time.perf_counter_ns() - start
            assert count == PAGE and buf[:] == payload(slot)
            times["fill" if slot < fills else "literal"].append(elapsed)
        tails = {kind: sorted(v)[(99 * len(v) + 99) // 100 - 1] for kind, v in times.items() if v}
        snapshot(stage, SLOTS, SLOTS - fills, tails)


def ranges(devices):
    """Time identical 8 MiB aligned streams. Keep preparation and verification outside the clock."""
    count = 2048
    rng = random.Random(3651)
    data = b"".join(bytes([i % 256]) * PAGE if i % 2 else rng.randbytes(PAGE) for i in range(count))
    targets = [(name, fd) for name, fd in devices if name in ("zram-zstd", "kernel-fill")]
    for pages in (1, 16, 64):
        size = pages * PAGE
        with mmap.mmap(-1, len(data)) as area:
            views = [memoryview(area)[i:i+size] for i in range(0, len(data), size)]
            for trial in range(3):
                for name, fd in targets[trial % 2:] + targets[:trial % 2]:
                    area[:] = data
                    t = time.perf_counter_ns()
                    for i, view in enumerate(views):
                        assert os.pwrite(fd, view, i * size) == size
                    write_ns = time.perf_counter_ns() - t
                    area[:] = bytes(len(data))
                    t = time.perf_counter_ns()
                    for i, view in enumerate(views):
                        assert os.preadv(fd, [view], i * size) == size
                    read_ns = time.perf_counter_ns() - t
                    assert area[:] == data
                    print("KERNEL_RANGE", json.dumps(dict(pages=pages, trial=trial, backend=name,
                          verified_bytes=len(data), write_ns=write_ns, read_ns=read_ns)), flush=True)
            for view in views:
                view.release()
