// Index entries are weak. Discarding the last logical page immediately frees
// its 4 KiB payload, even when its fingerprint remains in the lookup table.
use super::{literal_file, PAGE};
use std::hash::{BuildHasher, RandomState};
use std::mem::size_of;
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc, Weak,
};
use verdant_tensor::coded::{decide_uorc_challenger_admission, UorcChallengerVerdict};

pub(super) enum Capture<'a> {
    Borrowed(&'a [u8; PAGE]),
    Owned(Box<[u8; PAGE]>),
}
impl Capture<'_> {
    fn bytes(&self) -> &[u8; PAGE] {
        match self {
            Self::Borrowed(bytes) => bytes,
            Self::Owned(bytes) => bytes,
        }
    }
    fn into_box(self) -> Box<[u8; PAGE]> {
        match self {
            Self::Borrowed(bytes) => Box::new(*bytes),
            Self::Owned(bytes) => bytes,
        }
    }
}
enum Payload {
    Memory(Box<[u8; PAGE]>),
    File(literal_file::Slot),
}
pub(super) struct Literal {
    payload: Payload,
    live: Arc<AtomicU64>,
}
impl Literal {
    pub(super) fn read(&self, out: &mut [u8; PAGE]) -> Result<(), ()> {
        match &self.payload {
            Payload::Memory(data) => {
                verdant_tensor::memory::copy_storage_block(&data[..], out).map_err(|_| ())
            }
            Payload::File(slot) => slot.read(out).map_err(|_| ()),
        }
    }
    fn view<'a>(&'a self, scratch: &'a mut [u8; PAGE]) -> Option<&'a [u8; PAGE]> {
        match &self.payload {
            Payload::Memory(data) => Some(data),
            Payload::File(slot) => {
                slot.read(scratch).ok()?;
                Some(scratch)
            }
        }
    }
}
impl Drop for Literal {
    fn drop(&mut self) {
        self.live.fetch_sub(1, Ordering::Relaxed);
    }
}
type Entry = (u64, Weak<Literal>);
pub(super) struct Pool {
    pub(super) arena: Option<Arc<literal_file::Arena>>,
    keys: RandomState,
    buckets: Vec<Vec<Entry>>,
    live: Arc<AtomicU64>,
    pub(super) hits: u64,
    pub(super) residuals: u64,
}
impl Pool {
    pub(super) fn new() -> Self {
        Self {
            arena: None,
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
    /// An indexed or still-shared incumbent deserves exact admission before fitting.
    pub(super) fn has_candidate(
        &self,
        data: &[u8; PAGE],
        incumbent: Option<&Arc<Literal>>,
    ) -> bool {
        if !self.active() {
            return false;
        }
        let hash = self.fingerprint(data);
        incumbent.is_some_and(|base| Arc::strong_count(base) > 1)
            || self.buckets[hash as usize & (self.buckets.len() - 1)]
                .iter()
                .any(|(tag, weak)| *tag == hash && weak.strong_count() > 0)
    }
    pub(super) fn lookup(
        &mut self,
        hash: u64,
        data: &[u8; PAGE],
        incumbent: Option<&Arc<Literal>>,
    ) -> Option<super::Value> {
        let bucket = &self.buckets[hash as usize & (self.buckets.len() - 1)];
        let indexed = bucket
            .iter()
            .filter(|(tag, _)| *tag == hash)
            .map(|(_, weak)| weak);
        // Count owners before upgrading weak entries for this search.
        let exclusive = incumbent.filter(|base| Arc::strong_count(base) == 1);
        let extra = incumbent.filter(|base| {
            exclusive.is_none()
                && !indexed
                    .clone()
                    .any(|weak| weak.as_ptr() == Arc::as_ptr(base))
        });
        let mut best: Option<Residual> = None;
        for page in indexed.filter_map(Weak::upgrade).chain(extra.cloned()) {
            let mut scratch = [0; PAGE];
            let Some(bytes) = page.view(&mut scratch) else {
                continue;
            };
            let verdict = decide_uorc_challenger_admission(
                data,
                &bytes[..],
                true,
                PAGE,
                size_of::<Arc<Literal>>(),
            );
            if verdict == UorcChallengerVerdict::AdmitChallenger {
                self.hits += 1;
                return Some(super::Value::Shared(page));
            }
            // Replacing the last owner with a residual keeps an entire literal
            // alive solely for that edit. Exact reuse above still costs no write.
            if exclusive.is_some_and(|base| Arc::ptr_eq(base, &page)) {
                continue;
            }
            let limit = best
                .as_ref()
                .map_or(Residual::MAX_EDITS, |r| r.edits.len().saturating_sub(1));
            if let Some(residual) = Residual::fit(&page, bytes, data, limit) {
                best = Some(residual);
            }
        }
        let residual = best?;
        self.residuals += 1;
        Some(super::Value::Residual(Box::new(residual)))
    }
    pub(super) fn insert(
        &mut self,
        hash: Option<u64>,
        capture: Capture<'_>,
    ) -> Option<Arc<Literal>> {
        let hash = hash.unwrap_or_else(|| self.fingerprint(capture.bytes()));
        let payload = match &self.arena {
            Some(arena) => Payload::File(arena.store(capture.bytes())?),
            None => Payload::Memory(capture.into_box()),
        };
        let page = Arc::new(Literal {
            payload,
            live: self.live.clone(),
        });
        self.live.fetch_add(1, Ordering::Relaxed);
        self.resize_for_live_bases();
        if self.buckets.is_empty() {
            return Some(page);
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
        Some(page)
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
        self.arena
            .as_ref()
            .map_or(0, |arena| arena.metadata_bytes())
            + self.buckets.capacity() * size_of::<Vec<Entry>>()
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
    pub(super) base: Arc<Literal>,
    edits: Box<[(u16, u8)]>,
}
impl Residual {
    const MAX_EDITS: usize = 64;

    fn fit(
        base: &Arc<Literal>,
        bytes: &[u8; PAGE],
        data: &[u8; PAGE],
        limit: usize,
    ) -> Option<Self> {
        // Each differing word in this inexpensive sample
        // requires at least one edit, regardless of differences elsewhere.
        let unequal_words = bytes[8..]
            .chunks_exact(8)
            .zip(data[8..].chunks_exact(8))
            .take(Self::MAX_EDITS + 1)
            .filter(|(a, b)| a != b)
            .count();
        if unequal_words > limit {
            return None;
        }
        let mut edits = [(0u16, 0u8); Self::MAX_EDITS];
        let mut count = 0;
        for (row, (a, b)) in bytes
            .chunks_exact(super::SIDE)
            .zip(data.chunks_exact(super::SIDE))
            .enumerate()
        {
            if a == b {
                continue;
            }
            for (col, (&old, &new)) in a.iter().zip(b).enumerate() {
                if old == new {
                    continue;
                }
                if count == limit {
                    return None;
                }
                edits[count] = ((row * super::SIDE + col) as u16, old ^ new);
                count += 1;
            }
        }
        let candidate = Self {
            base: Arc::clone(base),
            edits: edits[..count].into(),
        };
        let cost = candidate.charge();
        if !matches!(
            vram_representation::run(false, PAGE as u64, PAGE as u64, PAGE as u64, cost as u64),
            vram_representation::Adt0::C5
        ) {
            return None;
        }
        // Admission and restore apply the same edits to the same immutable base.
        let mut check = *bytes;
        candidate.apply(&mut check);
        (decide_uorc_challenger_admission(data, &check, true, PAGE, cost)
            == UorcChallengerVerdict::AdmitChallenger)
            .then_some(candidate)
    }
    pub(super) fn decode(&self, out: &mut [u8; PAGE]) -> Result<(), ()> {
        self.base.read(out)?;
        self.apply(out);
        Ok(())
    }
    fn apply(&self, out: &mut [u8; PAGE]) {
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
    fn a_later_base_with_fewer_edits_wins() {
        let mut pool = Pool::new();
        let a = [5; PAGE];
        let mut b = a;
        b[100..180].fill(7);
        let mut near = a;
        near[100..150].fill(7);
        let tag = pool.fingerprint(&a);
        let _first = pool.insert(Some(tag), Capture::Borrowed(&a)).unwrap();
        let second = pool.insert(Some(tag), Capture::Borrowed(&b)).unwrap();
        let Some(super::super::Value::Residual(residual)) = pool.lookup(tag, &near, None) else {
            panic!("expected a sparse residual");
        };
        assert!(Arc::ptr_eq(&residual.base, &second));
        assert_eq!(residual.edits.len(), 30);
        assert!(
            matches!(pool.lookup(tag, &b, None), Some(super::super::Value::Shared(exact)) if Arc::ptr_eq(&exact, &second))
        );
    }

    #[test]
    fn large_dictionary_survives_growth_and_partial_trim() {
        let mut pool = Pool::new();
        let mut owners = Vec::new();
        for tag in 0..32768u64 {
            let mut bytes = Box::new([0; PAGE]);
            bytes[..8].copy_from_slice(&tag.to_le_bytes());
            owners.push(pool.insert(Some(tag), Capture::Owned(bytes)).unwrap());
        }
        for (tag, base) in owners.iter().enumerate() {
            assert!(matches!(
                pool.lookup(tag as u64, base.view(&mut [0; PAGE]).unwrap(), None),
                Some(super::super::Value::Shared(_))
            ));
        }
        let full = pool.buckets.len();
        // Force a capacity error before replacing a populated index.
        pool.try_rebuild(usize::MAX);
        assert_eq!(pool.buckets.len(), full);
        assert!(pool
            .lookup(1024, owners[1024].view(&mut [0; PAGE]).unwrap(), None)
            .is_some());
        owners.truncate(257);
        pool.resize_for_live_bases();
        assert!(pool.buckets.len() < full);
        assert_eq!(
            pool.buckets.iter().map(Vec::len).sum::<usize>(),
            owners.len()
        );
        for (tag, base) in owners.iter().enumerate() {
            assert!(matches!(
                pool.lookup(tag as u64, base.view(&mut [0; PAGE]).unwrap(), None),
                Some(super::super::Value::Shared(_))
            ));
        }
        owners.clear();
        pool.resize_for_live_bases();
        assert_eq!(pool.charge(), 0);
    }
}
