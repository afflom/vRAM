//! Lossless backing values for 4 KiB guest pages. FFI buffers are borrowed
//! for one call. The pager owns the store until its fault worker has joined.
mod lifecycle;
mod search;
mod selector;
use selector::Selector;
use std::{
    ffi::c_int,
    mem::{size_of, size_of_val},
    ptr, slice,
    sync::{
        atomic::{AtomicU64, Ordering},
        Mutex, OnceLock, RwLock,
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
            Self::Fill(b) => out.fill(*b),
            Self::Sparse(p) => {
                out.fill(p.base);
                for &(i, b) in p.entries.iter() {
                    out[usize::from(i)] = b;
                }
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
            Self::Program(_) | Self::Selector(_) | Self::Corrected(_) | Self::Raw(_) => {
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
    values: RwLock<Vec<Value>>,
    restores: AtomicU64,
    node_budget: usize,
    nodes_visited: AtomicU64,
    searches: AtomicU64,
    regions: Mutex<Vec<search::Region>>,
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
    let mut values = Vec::new();
    if nibble_table().is_none()
        || selector::mask_table().is_none()
        || count == 0
        || values.try_reserve_exact(count).is_err()
    {
        return ptr::null_mut();
    }
    values.resize_with(count, || Value::Fill(0));
    let mut regions = Vec::new();
    if adaptive && node_budget > 0 {
        let n = count.div_ceil(64);
        if regions.try_reserve_exact(n).is_err() {
            return ptr::null_mut();
        }
        regions.resize_with(n, search::Region::default);
    }
    Box::into_raw(Box::new(Store {
        values: RwLock::new(values),
        restores: AtomicU64::new(0),
        node_budget,
        nodes_visited: AtomicU64::new(0),
        searches: AtomicU64::new(0),
        regions: Mutex::new(regions),
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
    let Some(dst) = values.get_mut(page) else {
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
    let incumbent = Value::encode(dst, data);
    let mut regions = s.regions.lock().unwrap();
    let result = search::optimize(
        data,
        incumbent.charge(),
        s.node_budget,
        regions.get_mut(page / 64),
    );
    s.searches.fetch_add(result.probes, Ordering::Relaxed);
    s.nodes_visited
        .fetch_add(result.nodes as u64, Ordering::Relaxed);
    s.refits.fetch_add(result.refits, Ordering::Relaxed);
    s.skips.fetch_add(result.skips, Ordering::Relaxed);
    *dst = match result.program {
        Some(program) => Value::Program(Box::new(program)),
        None => incumbent,
    };
    0
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
    out[4] = (values.capacity() * size_of::<Value>() + size_of::<Store>()) as u64;
    let regions = s.regions.lock().unwrap();
    out[4] += (regions.capacity() * size_of::<search::Region>()
        + regions
            .iter()
            .map(search::Region::extra_bytes)
            .sum::<usize>()) as u64;
    for value in values.iter() {
        out[4] += value.charge() as u64;
        match value {
            Value::Fill(_) => out[0] += 1,
            Value::Packed(_) => out[1] += 1,
            Value::Raw(_) => out[2] += 1,
            Value::Sparse(_) => out[3] += 1,
            Value::Selector(_) | Value::Corrected(_) | Value::Program(_) => {}
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
