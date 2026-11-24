//! Lossless backing values for 4 KiB guest pages. FFI buffers are borrowed
//! for one call. The pager owns the store until its fault worker has joined.
mod directory;
mod lifecycle;
mod literals;
mod search;
mod selector;
use selector::Selector;
use std::{
    ffi::c_int,
    mem::{size_of, size_of_val},
    ptr, slice,
    sync::{
        atomic::{AtomicU64, Ordering},
        OnceLock, RwLock,
    },
};
use verdant_tensor::{
    coded::{
        contract_packed_animation_tracks, decide_uorc_challenger_admission, UorcChallengerVerdict,
    },
    linear::extract_packed_field,
    memory::copy_storage_block,
    prism_kernel::{codedTier1PackedAnimation, codedTierValid},
};

const PAGE: usize = 4096;
const SIDE: usize = 64;
// Hoist the release's Prism-generated packed-field evaluator across faults.
static NIBBLES: OnceLock<Option<[[u8; 2]; 256]>> = OnceLock::new();
fn nibble_table() -> Option<&'static [[u8; 2]; 256]> {
    NIBBLES
        .get_or_init(|| {
            let spec = codedTier1PackedAnimation().ok()?;
            if !codedTierValid(&spec).ok()? {
                return None;
            }
            let mut lut = [[0; 2]; 256];
            for (word, pair) in lut.iter_mut().enumerate() {
                for (lane, divisor) in [1, 16].into_iter().enumerate() {
                    pair[lane] =
                        u8::try_from(extract_packed_field(word as u64, 1, divisor, 16).ok()??)
                            .ok()?;
                }
            }
            Some(lut)
        })
        .as_ref()
}
struct Packed {
    table: [u8; 16],
    codes: [u8; PAGE / 2],
    exceptions: Box<[(u16, u8)]>,
}
struct Exceptions {
    base: u8,
    entries: Box<[(u16, u8)]>,
}
struct Corrected {
    base: Selector,
    edits: Box<[(u16, u8)]>,
}
enum Value {
    Program(Box<search::Program>),
    Fill(u8),
    Sparse(Box<Exceptions>),
    Packed(Box<Packed>),
    Raw(Box<[u8; PAGE]>),
    Shared(std::sync::Arc<literals::Literal>),
    Residual(Box<literals::Residual>),
    Selector(Box<Selector>),
    Corrected(Box<Corrected>),
}

impl Value {
    fn selector_base(&self) -> Option<&Selector> {
        match self {
            Self::Selector(model) => Some(model),
            Self::Corrected(p) => Some(&p.base),
            _ => None,
        }
    }

    fn charge(&self) -> usize {
        match self {
            Self::Fill(_) => 0,
            Self::Raw(_) => PAGE,
            Self::Residual(p) => p.charge(),
            Self::Shared(_) => 0, // The pool charges each live payload once.
            Self::Selector(_) => size_of::<Selector>(),
            Self::Corrected(p) => size_of::<Corrected>() + size_of_val(&*p.edits),
            Self::Sparse(p) => size_of::<Exceptions>() + size_of_val(&*p.entries),
            Self::Packed(p) => size_of::<Packed>() + size_of_val(&*p.exceptions),
            Self::Program(p) => p.charge(),
        }
    }

    fn encode(previous: &Value, bytes: &[u8; PAGE]) -> Self {
        use vram_representation::Adt0 as Plan;
        let choose = vram_representation::run;
        let absent = PAGE as u64;
        if matches!(
            choose(
                bytes.iter().all(|b| *b == bytes[0]),
                absent,
                absent,
                absent,
                absent
            ),
            Plan::C0
        ) {
            return Self::Fill(bytes[0]);
        }
        let mut histogram = [0usize; 256];
        for &b in bytes {
            histogram[usize::from(b)] += 1;
        }
        let mut symbols: [u8; 256] = std::array::from_fn(|i| i as u8);
        symbols.sort_unstable_by_key(|&c| (std::cmp::Reverse(histogram[usize::from(c)]), c));
        let matches: usize = symbols[..16]
            .iter()
            .map(|&b| histogram[usize::from(b)])
            .sum();
        let cost = size_of::<Packed>() + (PAGE - matches) * size_of::<(u16, u8)>();
        let base = symbols[0];
        let sparse_cost = size_of::<Exceptions>()
            + (PAGE - histogram[usize::from(base)]) * size_of::<(u16, u8)>();
        let mut base_bytes = [0; PAGE];
        let prior = previous.selector_base();
        let mut corrected_cost = PAGE;
        if let Some(model) = prior {
            if model.decode(&mut base_bytes).is_ok() {
                let count = bytes
                    .iter()
                    .zip(&base_bytes)
                    .filter(|(a, b)| a != b)
                    .count();
                if count == 0 {
                    return Value::Selector(Box::new(model.clone()));
                }
                corrected_cost = size_of::<Corrected>() + count * size_of::<(u16, u8)>();
            }
        }
        let select = |model_bytes: u64| {
            choose(
                false,
                sparse_cost as u64,
                cost as u64,
                model_bytes,
                corrected_cost as u64,
            )
        };
        if matches!(select(size_of::<Selector>() as u64), Plan::C4) {
            if let Some(model) = Selector::fit(bytes) {
                return Self::Selector(model);
            }
        }
        match select(absent) {
            Plan::C5 => {
                let edits = bytes
                    .iter()
                    .zip(&base_bytes)
                    .enumerate()
                    .filter_map(|(i, (&a, &b))| (a != b).then_some((i as u16, a)))
                    .collect();
                let value = Corrected {
                    base: prior.expect("model required by corrected cost").clone(),
                    edits,
                };
                return Self::Corrected(Box::new(value)).admit(bytes, corrected_cost);
            }
            Plan::C1 => {
                let entries = bytes
                    .iter()
                    .enumerate()
                    .filter(|(_, b)| **b != base)
                    .map(|(i, &b)| (i as u16, b))
                    .collect();
                return Self::Sparse(Box::new(Exceptions { base, entries }))
                    .admit(bytes, sparse_cost);
            }
            Plan::C2 => {}
            Plan::C3 => return Self::Raw(Box::new(*bytes)),
            _ => unreachable!("nonuniform page with no fresh selector candidate"),
        }
        let table: [u8; 16] = symbols[..16].try_into().unwrap();
        let mut index = [u8::MAX; 256];
        for (i, &b) in table.iter().enumerate() {
            index[usize::from(b)] = i as u8;
        }
        let mut exceptions = Vec::with_capacity(PAGE - matches);
        let mut codes = [0; PAGE / 2];
        for (i, &b) in bytes.iter().enumerate() {
            let mut code = index[usize::from(b)];
            if code == u8::MAX {
                exceptions.push((i as u16, b));
                code = 0;
            }
            codes[i / 2] |= code << ((i % 2) * 4);
        }
        let packed = Box::new(Packed {
            table,
            codes,
            exceptions: exceptions.into_boxed_slice(),
        });
        Self::Packed(packed).admit(bytes, cost)
    }

    fn admit(self, bytes: &[u8; PAGE], cost: usize) -> Self {
        let candidate = self;
        let mut check = [0; PAGE];
        if candidate.decode(&mut check).is_err() {
            return Self::Raw(Box::new(*bytes));
        }
        match decide_uorc_challenger_admission(bytes, &check, true, PAGE, cost) {
            UorcChallengerVerdict::AdmitChallenger => candidate,
            _ => Self::Raw(Box::new(*bytes)),
        }
    }

    fn decode(&self, out: &mut [u8; PAGE]) -> Result<(), ()> {
        match self {
            Self::Program(p) => return p.decode(out, false),
            Self::Residual(p) => p.decode(out),
            Self::Fill(b) => out.fill(*b),
            Self::Sparse(p) => {
                out.fill(p.base);
                for &(i, b) in p.entries.iter() {
                    out[usize::from(i)] = b;
                }
            }
            Self::Shared(page) => {
                copy_storage_block(&page.bytes[..], out).expect("equal page extents")
            }
            Self::Raw(data) => copy_storage_block(&data[..], out).expect("equal page extents"),
            Self::Selector(model) => return model.decode(out),
            Self::Corrected(p) => {
                p.base.decode(out)?;
                for &(i, b) in p.edits.iter() {
                    out[usize::from(i)] = b;
                }
            }
            Self::Packed(p) => {
                let lut = nibble_table().ok_or(())?;
                for (&code, pair) in p.codes.iter().zip(out.chunks_exact_mut(2)) {
                    let [a, b] = lut[usize::from(code)];
                    pair[0] = p.table[usize::from(a)];
                    pair[1] = p.table[usize::from(b)];
                }
                for &(i, b) in p.exceptions.iter() {
                    out[usize::from(i)] = b;
                }
            }
        }
        Ok(())
    }

    // One signed activation row times a 64 x 64 byte page in logical row order.
    fn project(&self, weights: &[i32; SIDE], out: &mut [i64; SIDE]) -> Result<bool, ()> {
        if let Self::Packed(p) = self {
            let table = p.table.map(i32::from);
            let v = contract_packed_animation_tracks(weights, 1, SIDE, SIDE, &table, &p.codes)
                .map_err(|_| ())?;
            out.copy_from_slice(&v);
            for &(i, b) in p.exceptions.iter() {
                let i = usize::from(i);
                out[i % SIDE] +=
                    i64::from(weights[i / SIDE]) * (i64::from(b) - i64::from(p.table[0]));
            }
            return Ok(true);
        }
        out.fill(0);
        match self {
            Self::Fill(v) => {
                out.fill(weights.iter().map(|x| i64::from(*x)).sum::<i64>() * i64::from(*v))
            }
            Self::Sparse(p) => {
                out.fill(weights.iter().map(|x| i64::from(*x)).sum::<i64>() * i64::from(p.base));
                for &(i, b) in p.entries.iter() {
                    let i = usize::from(i);
                    out[i % SIDE] +=
                        i64::from(weights[i / SIDE]) * (i64::from(b) - i64::from(p.base));
                }
            }
            Self::Program(_)
            | Self::Selector(_)
            | Self::Corrected(_)
            | Self::Raw(_)
            | Self::Residual(_)
            | Self::Shared(_) => {
                let mut expanded = [0u8; PAGE];
                let b = if let Self::Raw(raw) = self {
                    &**raw
                } else {
                    self.decode(&mut expanded)?;
                    &expanded
                };
                for (row, &w) in b.chunks_exact(SIDE).zip(weights) {
                    for (sum, &v) in out.iter_mut().zip(row) {
                        *sum += i64::from(w) * i64::from(v);
                    }
                }
            }
            Self::Packed(_) => unreachable!(),
        }
        Ok(false)
    }
}

pub struct Store {
    values: RwLock<directory::Directory>,
    restores: AtomicU64,
    literal_skips: AtomicU64,
    node_budget: usize,
    nodes_visited: AtomicU64,
    searches: AtomicU64,
    adaptive: bool,
    refits: AtomicU64,
    skips: AtomicU64,
}
#[no_mangle]
pub extern "C" fn store_new(count: usize) -> *mut Store {
    store_new_config(count, search::MAX_NODES, true)
}
#[no_mangle]
pub extern "C" fn store_new_with_search(count: usize, node_budget: usize) -> *mut Store {
    store_new_config(count, node_budget, false)
}
#[no_mangle]
pub extern "C" fn store_new_config(count: usize, node_budget: usize, adaptive: bool) -> *mut Store {
    if node_budget > search::MAX_NODES {
        return ptr::null_mut();
    }
    if count == 0 || nibble_table().is_none() || selector::mask_table().is_none() {
        return ptr::null_mut();
    }
    let Some(values) = directory::Directory::new(count) else {
        return ptr::null_mut();
    };
    Box::into_raw(Box::new(Store {
        values: RwLock::new(values),
        restores: AtomicU64::new(0),
        literal_skips: AtomicU64::new(0),
        node_budget,
        nodes_visited: AtomicU64::new(0),
        searches: AtomicU64::new(0),
        adaptive,
        refits: AtomicU64::new(0),
        skips: AtomicU64::new(0),
    }))
}
#[no_mangle]
pub unsafe extern "C" fn store_free(s: *mut Store) {
    drop(Box::from_raw(s));
}
#[no_mangle]
pub unsafe extern "C" fn store_put(s: &Store, page: usize, data: &[u8; PAGE]) -> c_int {
    let mut values = s.values.write().unwrap();
    let Some(dst) = values.get(page) else {
        return -1;
    };
    // Capture-all KVM may return an unchanged modeled frame. A matching expansion
    // authorizes retaining the exact backing, including its existing corrections.
    if dst.selector_base().is_some() || matches!(dst, Value::Program(_)) {
        let mut restored = [0; PAGE];
        if dst.decode(&mut restored).is_ok() && &restored == data {
            return 0;
        }
    }
    if data.iter().all(|&b| b == 0) {
        values.discard(page, page + 1);
        return 0;
    }
    let (group, literals) = values.write_group(page);
    let slot = &mut group.values[page % directory::GROUP];
    let hash = literals.active().then(|| literals.fingerprint(data));
    if let Some(shared) = hash.and_then(|h| literals.lookup(h, data)) {
        *slot = shared;
        return 0;
    }
    let credits = group.literal_credits;
    if matches!(
        vram_schedule::run(s.adaptive, credits, false),
        vram_schedule::Adt0::C2
    ) && matches!(slot, Value::Fill(_) | Value::Shared(_))
        && literals::diverse_axes(data)
    {
        group.literal_credits = vram_advance::run(false, credits);
        *slot = Value::Shared(literals.insert(hash, Box::new(*data)));
        s.literal_skips.fetch_add(1, Ordering::Relaxed);
        return 0;
    }
    let incumbent = Value::encode(slot, data);
    let result = search::optimize(
        data,
        incumbent.charge(),
        s.node_budget,
        s.adaptive.then_some(&mut group.region),
    );
    s.searches.fetch_add(result.probes, Ordering::Relaxed);
    s.nodes_visited
        .fetch_add(result.nodes as u64, Ordering::Relaxed);
    s.refits.fetch_add(result.refits, Ordering::Relaxed);
    s.skips.fetch_add(result.skips, Ordering::Relaxed);
    let candidate = match result.program {
        Some(program) => Value::Program(Box::new(program)),
        None => incumbent,
    };
    if let Value::Raw(bytes) = candidate {
        group.literal_credits = vram_advance::run(true, credits);
        *slot = Value::Shared(literals.insert(hash, bytes));
    } else {
        group.literal_credits = 0;
        *slot = candidate;
        literals.release_if_empty();
    }
    0
}
#[no_mangle]
pub unsafe extern "C" fn store_discard(s: &Store, start: usize, count: usize) -> c_int {
    let mut values = s.values.write().unwrap();
    if start > values.count || count > values.count - start {
        return -1;
    }
    values.discard(start, start + count);
    0
}
#[no_mangle]
pub extern "C" fn vram_host_auto_mib(available: u64) -> u64 {
    vram_host_size::run(available)
}
#[no_mangle]
pub unsafe extern "C" fn store_get(s: &Store, page: usize, data: &mut [u8; PAGE]) -> c_int {
    let values = s.values.read().unwrap();
    let Some(v) = values.get(page) else {
        return -1;
    };
    if v.decode(data).is_err() {
        return -1;
    }
    if v.selector_base().is_some() {
        s.restores.fetch_add(1, Ordering::Relaxed);
    }
    0
}
#[no_mangle]
pub unsafe extern "C" fn store_project(
    s: &Store,
    page: usize,
    weights: &[i32; SIDE],
    out: &mut [i64; SIDE],
) -> c_int {
    let values = s.values.read().unwrap();
    values
        .get(page)
        .and_then(|v| v.project(weights, out).ok())
        .map(i32::from)
        .unwrap_or(-1)
}
#[no_mangle]
pub unsafe extern "C" fn store_stats(s: &Store, out: *mut u64) {
    let out = slice::from_raw_parts_mut(out, 5);
    out.fill(0);
    let values = s.values.read().unwrap();
    out[4] = (size_of::<Store>() + values.metadata() + values.literals.charge()) as u64;
    out[0] = values.count as u64;
    for value in values.iter() {
        out[0] -= 1;
        out[4] += value.charge() as u64;
        match value {
            Value::Fill(_) => out[0] += 1,
            Value::Packed(_) => out[1] += 1,
            Value::Raw(_) | Value::Shared(_) => out[2] += 1,
            Value::Sparse(_) => out[3] += 1,
            Value::Selector(_) | Value::Corrected(_) | Value::Residual(_) | Value::Program(_) => {}
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn store_selector_stats(s: &Store, pages: &mut u64, restores: &mut u64) {
    *pages = s
        .values
        .read()
        .unwrap()
        .iter()
        .filter(|v| v.selector_base().is_some())
        .count() as u64;
    *restores = s.restores.load(Ordering::Relaxed);
}

#[no_mangle]
pub unsafe extern "C" fn store_literal_stats(
    s: &Store,
    unique: &mut u64,
    hits: &mut u64,
    skipped: &mut u64,
    residuals: &mut u64,
) {
    let values = s.values.read().unwrap();
    *unique = values.literals.live();
    *hits = values.literals.hits;
    *skipped = s.literal_skips.load(Ordering::Relaxed);
    *residuals = values.literals.residuals;
}
#[no_mangle]
pub unsafe extern "C" fn store_edits(s: &Store, pages: &mut u64, entries: &mut u64) {
    *pages = 0;
    *entries = 0;
    for value in s.values.read().unwrap().iter() {
        if let Value::Corrected(p) = value {
            *pages += 1;
            *entries += p.edits.len() as u64;
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn store_search_stats(
    s: &Store,
    pages: &mut u64,
    bytes: &mut u64,
    searches: &mut u64,
    nodes: &mut u64,
) {
    *pages = 0;
    *bytes = 0;
    for value in s.values.read().unwrap().iter() {
        if let Value::Program(p) = value {
            *pages += 1;
            *bytes += p.charge() as u64;
        }
    }
    *searches = s.searches.load(Ordering::Relaxed);
    *nodes = s.nodes_visited.load(Ordering::Relaxed);
}

#[no_mangle]
pub unsafe extern "C" fn store_feedback_stats(s: &Store, refits: &mut u64, skips: &mut u64) {
    *refits = s.refits.load(Ordering::Relaxed);
    *skips = s.skips.load(Ordering::Relaxed);
}

#[cfg(test)]
mod runtime_tests {
    use super::*;

    fn recover(value: &Value, bytes: &[u8; PAGE]) {
        let mut restored = [0; PAGE];
        value.decode(&mut restored).unwrap();
        assert_eq!(&restored, bytes);
    }

    #[test]
    fn correct_update_revert_and_replace() {
        let original = std::array::from_fn(|i| {
            let row = (i / SIDE) as u8;
            if i % 2 == 0 {
                row
            } else {
                !row
            }
        });
        let mut page = original;
        let mut value = Value::encode(&Value::Fill(0), &page);
        assert!(matches!(value, Value::Selector(_)));
        for (index, mask, edits) in [
            (0, 18, 1),
            (4095, 42, 2),
            (0, 7, 2),
            (0, 21, 1),
            (4095, 42, 0),
        ] {
            page[index] ^= mask;
            value = Value::encode(&value, &page);
            recover(&value, &page);
            if edits == 0 {
                assert!(matches!(value, Value::Selector(_)));
            } else {
                assert!(matches!(&value, Value::Corrected(p) if p.edits.len() == edits));
            }
        }
        assert_eq!(page, original);
        let entropy = std::array::from_fn(|i| (i % 256) as u8);
        value = Value::encode(&value, &entropy);
        assert!(matches!(value, Value::Raw(_)));
        recover(&value, &entropy);
        let rows = std::array::from_fn(|i| (i / SIDE) as u8);
        let value = Value::encode(&value, &rows);
        assert!(matches!(value, Value::Selector(_)));
        recover(&value, &rows);
    }

    #[test]
    fn range_boundaries_and_fault_transitions() {
        use super::lifecycle::*;
        assert!(!vram_range(u64::MAX - 4095, 4096, u64::MAX));
        assert!(vram_range(u64::MAX - 8191, 4096, u64::MAX));
        assert!(!vram_range(0, 0, u64::MAX));
        assert_eq!(vram_fault(false, false, true), 1);
        assert_eq!(vram_fault(false, true, true), 2);
        assert_eq!(vram_fault(false, false, false), 2);
        assert_eq!(vram_fault(true, true, true), 3);
        assert_eq!(vram_reclaim(true, false), 5);
        assert_eq!(vram_reclaim(true, true), 4);
        assert_eq!(vram_captured(false), 7);
        assert_eq!(vram_discarded(false), 7);
        assert!(!vram_trim(0, 100));
        assert!(vram_trim(99, 100));
    }
}

#[cfg(test)]
mod host_tests {
    use super::*;

    #[test]
    fn sparse_terabyte_directory_restores_implicit_zero_and_releases_empty_leaves() {
        unsafe {
            let n = 1usize << 28;
            let s = &*store_new_config(n, 49, true);
            let mut stats = [0; 5];
            store_stats(s, stats.as_mut_ptr());
            let initial = stats[4];
            assert!(initial < 256 * 1024);
            assert_eq!(stats[0], n as u64);
            let mut out = [1; PAGE];
            for at in [0, 63, 64, n - 2, n - 1] {
                assert_eq!(store_put(s, at, &[65; PAGE]), 0);
            }
            assert_eq!(store_discard(s, 63, 2), 0);
            assert_eq!(store_get(s, 64, &mut out), 0);
            assert_eq!(out, [0; PAGE]);
            assert_eq!(store_discard(s, n - 1, 2), -1);
            assert_eq!(store_get(s, n - 1, &mut out), 0);
            assert_eq!(out, [65; PAGE]);
            assert_eq!(store_discard(s, 0, n), 0);
            store_stats(s, stats.as_mut_ptr());
            assert_eq!(stats[4], initial);
            assert_eq!(stats[0], n as u64);
            store_free(s as *const _ as *mut _);
        }
    }

    #[test]
    fn final_literal_releases_index_on_overwrite_and_trim() {
        unsafe {
            let store = Box::from_raw(store_new(32768));
            let charge = || {
                let mut stats = [0; 5];
                store_stats(&store, stats.as_mut_ptr());
                stats[4]
            };
            let empty = charge();
            let fill = [7; PAGE];
            let dense = std::array::from_fn(|i| ((i * 17 + i / 7) % 251) as u8);
            let mut near = dense;
            near[77] ^= 43;
            assert_eq!(store_put(&store, 1, &fill), 0);
            let one_group = charge();
            assert_eq!(store_put(&store, 0, &dense), 0);
            assert_eq!(store_put(&store, 1, &near), 0);
            assert_eq!(store_discard(&store, 0, 1), 0);
            let mut restored = [0; PAGE];
            assert_eq!(store_get(&store, 1, &mut restored), 0);
            assert_eq!(near, restored);
            assert_eq!(store_put(&store, 1, &fill), 0);
            assert_eq!(charge(), one_group);

            assert_eq!(store_put(&store, 0, &dense), 0);
            assert_eq!(store_put(&store, 1, &dense), 0);
            assert_eq!(store.values.read().unwrap().literals.live(), 1);
            assert_eq!(store_discard(&store, 0, 2), 0);
            assert_eq!(charge(), empty);
            let mut hits = 0;
            let mut residuals = 0;
            store_literal_stats(&store, &mut 0, &mut hits, &mut 0, &mut residuals);
            assert_eq!((hits, residuals), (1, 1));
        }
    }

    #[test]
    fn fingerprint_collision_overwrite_and_discard_preserve_shared_bytes() {
        unsafe {
            let s = &*store_new(128);
            let a = std::array::from_fn(|i| ((i * 17 + i / 7) % 251) as u8);
            let mut b = a;
            for byte in &mut b[100..200] {
                *byte ^= 3;
            } // same sample, distinct literal
            for (i, bytes) in [(0, &a), (1, &a), (2, &b), (0, &b)] {
                assert_eq!(store_put(s, i, bytes), 0);
            }
            let (mut unique, mut hits, mut fast) = (0, 0, 0);
            store_literal_stats(s, &mut unique, &mut hits, &mut fast, &mut 0);
            assert_eq!(unique, 2);
            assert_eq!(hits, 2);
            let mut out = [0; PAGE];
            for (i, expected) in [(0, &b), (1, &a), (2, &b)] {
                assert_eq!(store_get(s, i, &mut out), 0);
                assert_eq!(&out, expected);
            }
            let mut near = a;
            near[77] ^= 43;
            assert_eq!(store_put(s, 3, &near), 0);
            assert_eq!(store_discard(s, 0, 3), 0);
            let mut residuals = 0;
            store_literal_stats(s, &mut unique, &mut hits, &mut fast, &mut residuals);
            assert_eq!((unique, residuals), (1, 1));
            assert_eq!(store_get(s, 3, &mut out), 0);
            assert_eq!(out, near);
            assert_eq!(store_discard(s, 3, 1), 0);
            store_literal_stats(s, &mut unique, &mut hits, &mut fast, &mut 0);
            assert_eq!(unique, 0);
            store_free(s as *const _ as *mut _);
        }
    }
}
