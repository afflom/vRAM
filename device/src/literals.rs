// Index entries are weak. Discarding the last logical page immediately frees
// its 4 KiB payload, even when its fingerprint remains in the lookup table.
use super::PAGE;
use std::hash::{BuildHasher, RandomState};
use std::mem::size_of;
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc, Weak,
};
use verdant_tensor::coded::{decide_uorc_challenger_admission, UorcChallengerVerdict};

pub(super) struct Literal {
    pub(super) bytes: Box<[u8; PAGE]>,
    live: Arc<AtomicU64>,
}
impl Drop for Literal {
    fn drop(&mut self) {
        self.live.fetch_sub(1, Ordering::Relaxed);
    }
}
type Entry = (u64, Weak<Literal>);
pub(super) struct Pool {
    keys: RandomState,
    buckets: Vec<Vec<Entry>>,
    live: Arc<AtomicU64>,
    pub(super) hits: u64,
    pub(super) residuals: u64,
}
impl Pool {
    pub(super) fn new() -> Self {
        Self {
            keys: RandomState::new(),
            buckets: Vec::new(),
            live: Arc::default(),
            hits: 0,
            residuals: 0,
        }
    }
    pub(super) fn fingerprint(&self, data: &[u8; PAGE]) -> u64 {
        self.keys
            .hash_one((&data[..8], &data[PAGE / 2..PAGE / 2 + 8], &data[PAGE - 8..]))
    }
    pub(super) fn active(&self) -> bool {
        !self.buckets.is_empty()
    }
    pub(super) fn lookup(&mut self, hash: u64, data: &[u8; PAGE]) -> Option<super::Value> {
        let bucket = &self.buckets[hash as usize & (self.buckets.len() - 1)];
        for (tag, weak) in bucket {
            if *tag != hash {
                continue;
            }
            let Some(page) = weak.upgrade() else {
                continue;
            };
            let verdict = decide_uorc_challenger_admission(
                data,
                &page.bytes[..],
                true,
                PAGE,
                size_of::<Arc<Literal>>(),
            );
            if verdict == UorcChallengerVerdict::AdmitChallenger {
                self.hits += 1;
                return Some(super::Value::Shared(page));
            }
        }
        let residual = bucket
            .iter()
            .filter(|(tag, _)| *tag == hash)
            .filter_map(|(_, weak)| Residual::fit(weak.upgrade()?, data))
            .min_by_key(Residual::charge)?;
        self.residuals += 1;
        Some(super::Value::Residual(Box::new(residual)))
    }
    pub(super) fn insert(&mut self, hash: Option<u64>, bytes: Box<[u8; PAGE]>) -> Arc<Literal> {
        let hash = hash.unwrap_or_else(|| self.fingerprint(&bytes));
        let page = Arc::new(Literal {
            bytes,
            live: self.live.clone(),
        });
        self.live.fetch_add(1, Ordering::Relaxed);
        self.resize_for_live_bases();
        if self.buckets.is_empty() {
            return page;
        }
        let index = hash as usize & (self.buckets.len() - 1);
        let bucket = &mut self.buckets[index];
        bucket.retain(|(_, weak)| weak.strong_count() != 0);
        let entry = (hash, Arc::downgrade(&page));
        // Bound full-page comparisons for identical samples. Unrelated
        // fingerprints in the same bucket keep their own candidates.
        if let Some(last) = bucket.iter_mut().filter(|(tag, _)| *tag == hash).nth(3) {
            *last = entry;
        } else if bucket.try_reserve(1).is_ok() {
            bucket.push(entry);
        }
        page
    }
    pub(super) fn live(&self) -> u64 {
        self.live.load(Ordering::Relaxed)
    }
    pub(super) fn resize_for_live_bases(&mut self) {
        let live = self.live();
        let mut count = self.buckets.len();
        loop {
            count = match vram_index_step::run(count as u64, live) {
                0 => {
                    self.buckets = Vec::new();
                    return;
                }
                1 => 4,
                2 => count * 2,
                3 => count / 2,
                4 => break,
                _ => unreachable!("invalid index policy step"),
            };
        }
        if count != self.buckets.len() {
            self.try_rebuild(count);
        }
    }
    fn try_rebuild(&mut self, count: usize) {
        // Keep the old index usable if memory pressure prevents a rebuild.
        // Every logical page owns its base independently of either index.
        let mut buckets: Vec<Vec<Entry>> = Vec::new();
        if buckets.try_reserve_exact(count).is_err() {
            return;
        }
        buckets.resize_with(count, Vec::new);
        for (hash, weak) in self.buckets.iter().flatten() {
            if weak.strong_count() != 0 {
                let bucket = &mut buckets[*hash as usize & (count - 1)];
                if bucket.try_reserve(1).is_err() {
                    return;
                }
                bucket.push((*hash, weak.clone()));
            }
        }
        self.buckets = buckets;
    }
    pub(super) fn charge(&self) -> usize {
        let live = self.live() as usize;
        let allocated: usize = self.buckets.iter().map(Vec::capacity).sum();
        let dead = self
            .buckets
            .iter()
            .flatten()
            .filter(|(_, weak)| weak.strong_count() == 0)
            .count();
        self.buckets.capacity() * size_of::<Vec<Entry>>()
            + allocated * size_of::<Entry>()
            + live * PAGE
            + (live + dead) * (size_of::<Literal>() + 2 * size_of::<usize>())
    }
}

// Only schedule an inexpensive literal capture when both axes exhibit high
// sample diversity. The full Prism probe returns as its credits expire.
pub(super) fn diverse_axes(data: &[u8; PAGE]) -> bool {
    [1, super::SIDE].into_iter().all(|stride| {
        let mut seen = [0u64; 4];
        for lane in 0..32 {
            let byte = usize::from(data[lane * stride]);
            seen[byte / 64] |= 1 << (byte % 64);
        }
        seen.iter().map(|word| word.count_ones()).sum::<u32>() > 16
    })
}

pub(super) struct Residual {
    base: Arc<Literal>,
    edits: Box<[(u16, u8)]>,
}
impl Residual {
    fn fit(base: Arc<Literal>, data: &[u8; PAGE]) -> Option<Self> {
        // A short edit list bounds unsuccessful fits on pages with common headers.
        // Each nonzero byte is a coordinate of the XOR residual over GF(2).
        let mut edits = Vec::new();
        for (i, (&a, &b)) in base.bytes.iter().zip(data).enumerate() {
            if a != b {
                if edits.len() == 64 {
                    return None;
                }
                edits.push((i as u16, a ^ b));
            }
        }
        let candidate = Self {
            base,
            edits: edits.into_boxed_slice(),
        };
        let cost = candidate.charge();
        if !matches!(
            vram_representation::run(false, PAGE as u64, PAGE as u64, PAGE as u64, cost as u64),
            vram_representation::Adt0::C5
        ) {
            return None;
        }
        let mut check = [0u8; PAGE];
        candidate.decode(&mut check);
        (decide_uorc_challenger_admission(data, &check, true, PAGE, cost)
            == UorcChallengerVerdict::AdmitChallenger)
            .then_some(candidate)
    }
    pub(super) fn decode(&self, out: &mut [u8; PAGE]) {
        verdant_tensor::memory::copy_storage_block(&self.base.bytes[..], out)
            .expect("equal page extents");
        for &(i, delta) in &self.edits {
            out[usize::from(i)] ^= delta;
        }
    }
    pub(super) fn charge(&self) -> usize {
        size_of::<Self>() + std::mem::size_of_val(&*self.edits)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn large_dictionary_survives_growth_and_partial_trim() {
        let mut pool = Pool::new();
        let mut owners = Vec::new();
        for tag in 0..32768u64 {
            let mut bytes = Box::new([0; PAGE]);
            bytes[..8].copy_from_slice(&tag.to_le_bytes());
            owners.push(pool.insert(Some(tag), bytes));
        }
        for (tag, base) in owners.iter().enumerate() {
            assert!(matches!(
                pool.lookup(tag as u64, &base.bytes),
                Some(super::super::Value::Shared(_))
            ));
        }
        let full = pool.buckets.len();
        // Force a capacity error before replacing a populated index.
        pool.try_rebuild(usize::MAX);
        assert_eq!(pool.buckets.len(), full);
        assert!(pool.lookup(1024, &owners[1024].bytes).is_some());
        owners.truncate(257);
        pool.resize_for_live_bases();
        assert!(pool.buckets.len() < full);
        assert_eq!(
            pool.buckets.iter().map(Vec::len).sum::<usize>(),
            owners.len()
        );
        for (tag, base) in owners.iter().enumerate() {
            assert!(matches!(
                pool.lookup(tag as u64, &base.bytes),
                Some(super::super::Value::Shared(_))
            ));
        }
        owners.clear();
        pool.resize_for_live_bases();
        assert_eq!(pool.charge(), 0);
    }
}
