"""Single-page latency workload supplied by the operator. Runs only in the disposable guest."""
import errno
import json
import mmap
import os
import random
import statistics
import sys
import time

size, pages = 4096, 2048
order = list(range(pages))
random.Random(14).shuffle(order)
buf = mmap.mmap(-1, size)
literal = os.path.exists("/kernel-literal.ko")
if literal:
    paths = (("zram-zstd", "/dev/zram0"), ("kernel-literal", "/dev/vramlit0"), ("vram-nbd", "/dev/nbd0"))
else:
    paths = (("zram-zstd", "/dev/zram0"), ("vram", "/dev/nbd1"), ("control", "/dev/nbd0"))
devices = [(label, os.open(path, os.O_RDWR | os.O_DIRECT | os.O_EXCL)) for label, path in paths]
if literal:
    fd = dict(devices)["kernel-literal"]
    end = os.lseek(fd, 0, os.SEEK_END)
    assert end == 64 << 20
    for offset, last_value in ((0, 19), (end - size, 83)):
        assert os.preadv(fd, [buf], offset) == size and buf[:] == bytes(size)
        for value in (247, last_value):
            buf[:] = bytes([value]) * size
            assert os.pwrite(fd, buf, offset) == size
            buf[:] = bytes(size)
            assert os.preadv(fd, [buf], offset) == size and buf[:] == bytes([value]) * size
    try:
        os.pwrite(fd, buf, end)
    except OSError as error:
        assert error.errno == errno.ENOSPC
    else:
        raise AssertionError("write past end succeeded")
    assert os.preadv(fd, [buf], end) == 0
    assert os.preadv(fd, [buf], 0) == size and buf[:] == bytes([19]) * size
    with open("/sys/module/vram_literal/parameters/policy") as policy:
        print("LITERAL_POLICY", policy.read().strip(), flush=True)
print("INTERPRETER", sys.version.split()[0], flush=True)
clock = None
if os.path.exists("/store-clock"):
    # -I excludes the guest script directory from the interpreter import path.
    sys.path.insert(0, "/")
    from swap_clock import StoreClock
    clock = StoreClock()

for trial in range(3):
    for k, profile in enumerate(("patterned", "dense")):
        blocks = [os.urandom(64)*64 if k == 0 else os.urandom(size) for _ in order]
        base = 256 + pages * (trial*2 + k)
        for label, fd in devices[trial:] + devices[:trial]:
            tracing = clock is not None and label == "control"
            writes, reads = [], []
            for index, page in enumerate(blocks):
                buf[:] = page
                offset = (base + index)*size
                t = time.perf_counter_ns()
                count = os.pwrite(fd, buf, offset)
                writes.append(time.perf_counter_ns() - t)
                assert count == size
                if tracing:
                    clock.add(trial, profile, "write", base + index, t, writes[-1])
            for index in order:
                offset = (base + index)*size
                t = time.perf_counter_ns()
                count = os.preadv(fd, [buf], offset)
                reads.append(time.perf_counter_ns() - t)
                assert count == size and buf[:] == blocks[index]
                if tracing:
                    clock.add(trial, profile, "read", base + index, t, reads[-1])
            for op, samples in (("write", writes), ("read", reads)):
                print("PAGE_LATENCY", json.dumps([trial, label, profile, op, statistics.median(samples),
                      sorted(samples)[(len(samples)*99+99)//100-1]]), flush=True)
# Overwrite the same addresses across rounds with unrelated shapes. Keep these
# observations separate from the four operator-supplied benchmark cells.
rng = random.Random(7248)
families = ("sparse", "selector", "period97", "perturbed", "independent", "fill")
mixed_order = list(range(384))
rng.shuffle(mixed_order)
for trial in range(3):
    blocks = []
    for i in range(len(mixed_order)):
        family = families[(i + trial) % len(families)]
        if family == "sparse":
            page = bytearray([rng.randrange(256)]) * size
            for pos in rng.sample(range(size), 8):
                page[pos] ^= 1
        elif family == "selector":
            mask = rng.getrandbits(64)
            pairs = rng.randbytes(128)
            page = bytes(pairs[2*(j//64) + ((mask >> (j%64)) & 1)] for j in range(size))
        elif family == "period97":
            page = (rng.randbytes(97) * 43)[:size]
        elif family == "perturbed":
            period = rng.choice((2, 8, 128, 512, 2048))
            page = bytearray(rng.randbytes(period) * (size // period))
            page[rng.randrange(size)] ^= 1
        elif family == "independent":
            page = rng.randbytes(size)
        else:
            page = bytes([rng.randrange(256)]) * size
        blocks.append(bytes(page))
    for label, fd in devices[trial:] + devices[:trial]:
        tracing = clock is not None and label == "control"
        for op in ("write", "read"):
            times = {name: [] for name in families}
            for i in (range(len(blocks)) if op == "write" else mixed_order):
                offset = (14000 + i)*size
                if op == "write":
                    buf[:] = blocks[i]
                    t = time.perf_counter_ns()
                    count = os.pwrite(fd, buf, offset)
                else:
                    t = time.perf_counter_ns()
                    count = os.preadv(fd, [buf], offset)
                elapsed = time.perf_counter_ns() - t
                assert count == size and (op == "write" or buf[:] == blocks[i])
                family = families[(i + trial) % len(families)]
                times[family].append(elapsed)
                if tracing:
                    clock.add(trial, family, op, 14000 + i, t, elapsed)
            for name, samples in times.items():
                print("MIXED_LATENCY", json.dumps([trial, label, name, op, statistics.median(samples),
                      sorted(samples)[(len(samples)*99+99)//100-1]]), flush=True)
if os.path.exists("/swap_read_windows.py"):
    sys.path.insert(0, "/")
    from swap_read_windows import measure
    measure(devices)
for _, fd in devices:
    os.close(fd)

if clock is not None:
    clock.emit()
