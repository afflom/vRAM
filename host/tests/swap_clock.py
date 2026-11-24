"""Guest-side observer for the separately linked store clock."""
import json
import mmap
import time


class StoreClock:
    def __init__(self):
        if time.get_clock_info("perf_counter").implementation != "clock_gettime(CLOCK_MONOTONIC)":
            raise RuntimeError("Server and caller require the same monotonic clock")
        with open("/store-clock", "rb") as f:
            self.record = memoryview(mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)).cast("Q")
        self.rows = {}
        self.sequence = None

    def add(self, trial, profile, op, page, start, elapsed):
        sequence, observed_page, write, enter, leave = self.record
        if ((self.sequence is not None and sequence != self.sequence + 1) or
                (observed_page, write) != (page, int(op == "write")) or
                not start <= enter <= leave <= start + elapsed):
            raise ValueError(f"Unmatched store call at {trial}/{profile}/{op}/{page}")
        self.sequence = sequence
        self.rows.setdefault((trial, profile, op), []).append(
            [sequence, page, start, enter, leave, start + elapsed])

    def emit(self):
        # Stage the trace after measurement. PID 1 sends it over a dedicated channel.
        with open("/path-samples.json", "x") as target:
            json.dump([[*key, rows] for key, rows in self.rows.items()], target, separators=(",", ":"))
