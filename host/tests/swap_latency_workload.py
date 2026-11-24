"""Single-page latency workload supplied by the operator. Runs only in the disposable guest."""
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
devices = [(label, os.open(path, os.O_RDWR | os.O_DIRECT | os.O_EXCL))
           for label, path in (("zram-zstd", "/dev/zram0"), ("vram", "/dev/nbd1"), ("control", "/dev/nbd0"))]
print("INTERPRETER", sys.version.split()[0], flush=True)
for trial in range(3):
    for k, profile in enumerate(("patterned", "dense")):
        blocks = [os.urandom(64)*64 if k == 0 else os.urandom(size) for _ in order]
        base = 256 + pages * (trial*2 + k)
        for label, fd in devices[trial:] + devices[:trial]:
            writes, reads = [], []
            for index, page in enumerate(blocks):
                buf[:] = page
                offset = (base + index)*size
                t = time.perf_counter_ns()
                count = os.pwrite(fd, buf, offset)
                writes.append(time.perf_counter_ns() - t)
                assert count == size
            for index in order:
                offset = (base + index)*size
                t = time.perf_counter_ns()
                count = os.preadv(fd, [buf], offset)
                reads.append(time.perf_counter_ns() - t)
                assert count == size and buf[:] == blocks[index]
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
                times[families[(i + trial) % len(families)]].append(elapsed)
            for name, samples in times.items():
                print("MIXED_LATENCY", json.dumps([trial, label, name, op, statistics.median(samples),
                      sorted(samples)[(len(samples)*99+99)//100-1]]), flush=True)
for _, fd in devices:
    os.close(fd)
