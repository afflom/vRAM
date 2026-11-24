"""Guest workload for direct reads that span one or more Verdant frames."""
import json
import mmap
import os
import random
import statistics
import time


def measure(devices):
    page = 4096
    pages = 1024
    base = 256 * page
    rng = random.Random(539)
    blocks = [rng.randbytes(page) if i % 2 else rng.randbytes(32) * 128 for i in range(pages)]
    wanted = b"".join(blocks)
    sizes = (1, 2, 16, 64, 65, 128)
    with mmap.mmap(-1, len(wanted)) as backing:
        backing[:] = wanted
        for _, fd in devices:
            assert os.pwrite(fd, backing, base) == len(backing)
    for trial in range(3):
        for size in sizes:
            length = size * page
            starts = rng.sample(range(pages - size + 1), 128)
            with mmap.mmap(-1, length) as buf:
                for label, fd in devices[trial:] + devices[:trial]:
                    times = []
                    for first in starts:
                        offset = first * page
                        start = time.perf_counter_ns()
                        count = os.preadv(fd, [buf], base + offset)
                        times.append(time.perf_counter_ns() - start)
                        assert count == length and buf[:] == wanted[offset:offset + length]
                    print("WINDOW_LATENCY", json.dumps([trial, label, f"pages{size}", "read",
                          statistics.median(times), sorted(times)[126]]), flush=True)
