// Enabled for diagnostic launches only. Totals are thread time and can overlap.
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant};

#[derive(Default)]
pub(super) struct Profile {
    pub prepared: AtomicU64,
    pub credit_replays: AtomicU64,
    pub encoding_reuses: AtomicU64,
    pub fit_ns: AtomicU64,
    pub overlap_ns: AtomicU64,
    pub snapshot_ns: AtomicU64,
    pub reacquire_ns: AtomicU64,
    pub validation_ns: AtomicU64,
    commits: Mutex<CommitClock>,
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

// put_locked invocations are mutually exclusive. Their cumulative active time
// therefore measures the union of commit intervals, including a live interval.
#[derive(Default)]
struct CommitClock {
    finished: Duration,
    started: Option<Instant>,
}
impl CommitClock {
    fn at(&self, now: Instant) -> Duration {
        self.finished + self.started.map_or(Duration::ZERO, |t| now - t)
    }
}

pub(super) struct CommitInterval<'a>(&'a Profile);
impl Drop for CommitInterval<'_> {
    fn drop(&mut self) {
        let mut clock = self.0.commits.lock().unwrap();
        clock.finished = clock.at(Instant::now());
        clock.started = None;
    }
}
impl Profile {
    pub(super) fn begin_commit(&self) -> CommitInterval<'_> {
        let mut clock = self.commits.lock().unwrap();
        debug_assert!(clock.started.is_none());
        clock.started = Some(Instant::now());
        CommitInterval(self)
    }

    fn sample(&self) -> (Instant, Duration) {
        let clock = self.commits.lock().unwrap();
        let now = Instant::now();
        (now, clock.at(now))
    }

    pub(super) fn measure_fit<T>(&self, fit: impl FnOnce() -> T) -> T {
        let (start, before) = self.sample();
        let result = fit();
        let (end, after) = self.sample();
        self.fit_ns
            .fetch_add((end - start).as_nanos() as u64, Ordering::Relaxed);
        self.overlap_ns
            .fetch_add((after - before).as_nanos() as u64, Ordering::Relaxed);
        self.prepared.fetch_add(1, Ordering::Relaxed);
        result
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fit_overlap_counts_only_intersecting_commit_time() {
        let profile = Profile::default();
        {
            let _commit = profile.begin_commit();
            profile.measure_fit(std::thread::yield_now);
        }
        let fully_overlapped = profile.fit_ns.load(Ordering::Relaxed);
        assert!(fully_overlapped > 0);
        assert_eq!(profile.overlap_ns.load(Ordering::Relaxed), fully_overlapped);
        profile.measure_fit(std::thread::yield_now);
        assert!(profile.fit_ns.load(Ordering::Relaxed) > fully_overlapped);
        assert_eq!(profile.overlap_ns.load(Ordering::Relaxed), fully_overlapped);
    }
}
