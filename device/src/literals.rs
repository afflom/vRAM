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
type Entry = Option<(u64, Weak<Literal>)>;
pub(super) struct Pool {
    keys: RandomState,
    slots: Vec<Entry>,
    limit: usize,
    live: Arc<AtomicU64>,
    pub(super) hits: u64,
    pub(super) residuals: u64,
}
impl Pool {
    pub(super) fn new(pages: usize) -> Self {
        Self {
            keys: RandomState::new(),
            slots: Vec::new(),
            limit: pages.clamp(4, 16384).next_power_of_two(),
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
        !self.slots.is_empty()
    }
    pub(super) fn lookup(&mut self, hash: u64, data: &[u8; PAGE]) -> Option<super::Value> {
        let index = (hash as usize & (self.limit - 1)) & !3;
        for (tag, weak) in self.slots[index..index + 4].iter().flatten() {
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
        let residual = self.slots[index..index + 4]
            .iter()
            .flatten()
            .filter(|(tag, _)| *tag == hash)
            .filter_map(|(_, weak)| Residual::fit(weak.upgrade()?, data))
            .min_by_key(Residual::charge)?;
        self.residuals += 1;
        Some(super::Value::Residual(Box::new(residual)))
    }
    pub(super) fn insert(&mut self, hash: Option<u64>, bytes: Box<[u8; PAGE]>) -> Arc<Literal> {
        let hash = hash.unwrap_or_else(|| self.fingerprint(&bytes));
        if self.slots.is_empty() {
            self.slots.resize_with(self.limit, || None);
        }
        let page = Arc::new(Literal {
            bytes,
            live: self.live.clone(),
        });
        self.live.fetch_add(1, Ordering::Relaxed);
        let index = (hash as usize & (self.limit - 1)) & !3;
        let bucket = &mut self.slots[index..index + 4];
        let way = bucket
            .iter()
            .position(|entry| {
                entry
                    .as_ref()
                    .is_none_or(|(_, weak)| weak.strong_count() == 0)
            })
            .unwrap_or((hash >> 32) as usize & 3);
        bucket[way] = Some((hash, Arc::downgrade(&page)));
        page
    }
    pub(super) fn live(&self) -> u64 {
        self.live.load(Ordering::Relaxed)
    }
    pub(super) fn release_if_empty(&mut self) {
        // Residuals also own bases. Their last drop retires the index just as
        // removing the final exact reference does. Keep lifetime hit counters.
        if self.active() && self.live() == 0 {
            self.slots = Vec::new();
        }
    }
    pub(super) fn charge(&self) -> usize {
        let live = self.live() as usize;
        let dead = self
            .slots
            .iter()
            .flatten()
            .filter(|(_, w)| w.strong_count() == 0)
            .count();
        self.slots.capacity() * size_of::<Entry>()
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
