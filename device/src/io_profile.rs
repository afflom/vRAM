// Enabled for diagnostic launches only. Totals are thread time and can overlap.
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Instant;

#[derive(Default)]
pub(super) struct Profile {
    pub prepared: AtomicU64,
    pub used: AtomicU64,
    pub read_lock_ns: AtomicU64,
    pub write_lock_ns: AtomicU64,
    pub pread_ns: AtomicU64,
    pub pwrite_ns: AtomicU64,
}

pub(super) fn measured<T>(counter: Option<&AtomicU64>, action: impl FnOnce() -> T) -> T {
    let Some(counter) = counter else {
        return action();
    };
    let start = Instant::now();
    let result = action();
    counter.fetch_add(start.elapsed().as_nanos() as u64, Ordering::Relaxed);
    result
}
