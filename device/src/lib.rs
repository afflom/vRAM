//! Lossless backing values for 4 KiB guest pages. FFI buffers are borrowed
//! for one call. The pager owns the store until its fault worker has joined.
mod directory;
mod io_profile;
mod lifecycle;
mod literal_file;
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
        Arc, OnceLock, RwLock,
    },
};
use verdant_tensor::{
    coded::{
        contract_packed_animation_tracks, decide_uorc_challenger_admission, UorcChallengerVerdict,
    },
    linear::extract_packed_field,
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
    Shared(Arc<literals::Literal>),
    Residual(Box<literals::Residual>),
    Selector(Box<Selector>),
    Corrected(Box<Corrected>),
}

/// The packed policy only consumes the first sixteen symbols of the total ranking.
fn palette(histogram: &[usize; 256]) -> [u8; 16] {
    let rank = |&c: &u8| (std::cmp::Reverse(histogram[usize::from(c)]), c);
    let mut symbols: [u8; 256] = std::array::from_fn(|i| i as u8);
    symbols.select_nth_unstable_by_key(15, rank);
    let mut table: [u8; 16] = symbols[..16].try_into().unwrap();
    table.sort_unstable_by_key(rank);
    table
}

impl Value {
    fn literal_base(&self) -> Option<&Arc<literals::Literal>> {
        match self {
            Self::Shared(base) => Some(base),
            Self::Residual(residual) => Some(&residual.base),
            _ => None,
        }
    }

    fn replace(&mut self, next: Self, pool: &mut literals::Pool) {
        // The directory write lock excludes changes to logical ownership.
        // A next value referring to this same base already owns a strong reference.
        let retires_base = self
            .literal_base()
            .is_some_and(|base| Arc::strong_count(base) == 1);
        *self = next;
        if retires_base {
            pool.resize_for_live_bases();
        }
    }

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
            Self::Residual(p) => p.charge(),
            Self::Shared(_) => 0, // The pool charges each live payload once.
            Self::Selector(_) => size_of::<Selector>(),
            Self::Corrected(p) => size_of::<Corrected>() + size_of_val(&*p.edits),
            Self::Sparse(p) => size_of::<Exceptions>() + size_of_val(&*p.entries),
            Self::Packed(p) => size_of::<Packed>() + size_of_val(&*p.exceptions),
            Self::Program(p) => p.charge(),
        }
    }

    /// None selects the submitted literal bytes, borrowed until the commit finishes.
    fn encode(prior: Option<&Selector>, bytes: &[u8; PAGE]) -> Option<Self> {
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
            return Some(Self::Fill(bytes[0]));
        }
        let mut histogram = [0usize; 256];
        for &b in bytes {
            histogram[usize::from(b)] += 1;
        }
        let table = palette(&histogram);
        let matches: usize = table.iter().map(|&b| histogram[usize::from(b)]).sum();
        let cost = size_of::<Packed>() + (PAGE - matches) * size_of::<(u16, u8)>();
        let base = table[0];
        let sparse_cost = size_of::<Exceptions>()
            + (PAGE - histogram[usize::from(base)]) * size_of::<(u16, u8)>();
        let mut base_bytes = [0; PAGE];
        let mut corrected_cost = PAGE;
        if let Some(model) = prior {
            if model.decode(&mut base_bytes).is_ok() {
                let count = bytes
                    .iter()
                    .zip(&base_bytes)
                    .filter(|(a, b)| a != b)
                    .count();
                if count == 0 {
                    return Some(Self::Selector(Box::new(model.clone())));
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
                return Some(Self::Selector(model));
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
            Plan::C3 => return None,
            _ => unreachable!("nonuniform page with no fresh selector candidate"),
        }
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

    fn admit(self, bytes: &[u8; PAGE], cost: usize) -> Option<Self> {
        let candidate = self;
        let mut check = [0; PAGE];
        if candidate.decode(&mut check).is_err() {
            return None;
        }
        match decide_uorc_challenger_admission(bytes, &check, true, PAGE, cost) {
            UorcChallengerVerdict::AdmitChallenger => Some(candidate),
            _ => None,
        }
    }

    fn decode(&self, out: &mut [u8; PAGE]) -> Result<(), ()> {
        match self {
            Self::Program(p) => return p.decode(out, false),
            Self::Residual(p) => return p.decode(out),
            Self::Fill(b) => out.fill(*b),
            Self::Sparse(p) => {
                out.fill(p.base);
                for &(i, b) in p.entries.iter() {
                    out[usize::from(i)] = b;
                }
            }
            Self::Shared(page) => return page.read(out),
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

    fn project(&self, weights: &[[i32; SIDE]], out: &mut [[i64; SIDE]]) -> Result<bool, ()> {
        if let Self::Packed(p) = self {
            let table = p.table.map(i32::from);
            let product = contract_packed_animation_tracks(
                weights.as_flattened(),
                weights.len(),
                SIDE,
                SIDE,
                &table,
                &p.codes,
            )
            .map_err(|_| ())?;
            out.as_flattened_mut().copy_from_slice(&product);
            for &(i, b) in p.exceptions.iter() {
                let i = usize::from(i);
                let delta = i64::from(b) - i64::from(p.table[0]);
                for (w, sums) in weights.iter().zip(out.iter_mut()) {
                    sums[i % SIDE] += i64::from(w[i / SIDE]) * delta;
                }
            }
            return Ok(true);
        }
        let mut expanded = [0; PAGE];
        if !matches!(self, Self::Fill(_) | Self::Sparse(_)) {
            self.decode(&mut expanded)?;
        }
        for (w, sums) in weights.iter().zip(out) {
            match self {
                Self::Fill(v) => {
                    sums.fill(w.iter().map(|x| i64::from(*x)).sum::<i64>() * i64::from(*v))
                }
                Self::Sparse(p) => {
                    sums.fill(w.iter().map(|x| i64::from(*x)).sum::<i64>() * i64::from(p.base));
                    for &(i, b) in p.entries.iter() {
                        let i = usize::from(i);
                        sums[i % SIDE] +=
                            i64::from(w[i / SIDE]) * (i64::from(b) - i64::from(p.base));
                    }
                }
                _ => {
                    sums.fill(0);
                    for (row, &weight) in expanded.chunks_exact(SIDE).zip(w) {
                        for (sum, &byte) in sums.iter_mut().zip(row) {
                            *sum += i64::from(weight) * i64::from(byte);
                        }
                    }
                }
            }
        }
        Ok(false)
    }
}

pub struct Store {
    count: usize,
    waiting_writers: AtomicU64,
    profile: Option<Arc<io_profile::Profile>>,
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
pub extern "C" fn vram_capacity_ceiling(
    platform_mib: u64,
    guest_mib: u64,
    backing_bytes: u64,
) -> u64 {
    vram_capacity_ceiling::run(platform_mib, guest_mib, backing_bytes)
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
    let values = directory::Directory::new();
    Box::into_raw(Box::new(Store {
        count,
        waiting_writers: AtomicU64::new(0),
        profile: None,
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
/// Construct a fresh store whose immutable literal payloads use a private,
/// preallocated regular file. The caller keeps ownership of the input fd.
#[no_mangle]
pub unsafe extern "C" fn store_new_file(count: usize, fd: c_int) -> *mut Store {
    use std::os::fd::BorrowedFd;
    if fd < 0 {
        return ptr::null_mut();
    }
    let Ok(owned) = BorrowedFd::borrow_raw(fd).try_clone_to_owned() else {
        return ptr::null_mut();
    };
    let Some(arena) = literal_file::Arena::new(owned.into(), count) else {
        return ptr::null_mut();
    };
    let store = store_new(count);
    if !store.is_null() {
        (*store).profile = arena.profile.clone();
        (*store).values.get_mut().unwrap().literals.arena = Some(arena);
    }
    store
}

#[no_mangle]
pub unsafe extern "C" fn store_file_stats(s: &Store, reads: &mut u64, writes: &mut u64) {
    let values = s.values.read().unwrap();
    let arena = values.literals.arena.as_ref();
    *reads = arena.map_or(0, |a| a.reads.load(Ordering::Relaxed));
    *writes = arena.map_or(0, |a| a.writes.load(Ordering::Relaxed));
}

#[no_mangle]
pub unsafe extern "C" fn store_free(s: *mut Store) {
    drop(Box::from_raw(s));
}
impl Store {
    fn write_directory(&self) -> std::sync::RwLockWriteGuard<'_, directory::Directory> {
        io_profile::measured(self.profile.as_ref().map(|p| &p.write_lock_ns), || {
            self.values.write().unwrap()
        })
    }
    fn wait_for_writer(
        &self,
        after_fit: bool,
    ) -> std::sync::RwLockWriteGuard<'_, directory::Directory> {
        self.waiting_writers.fetch_add(1, Ordering::Relaxed);
        let values = match self.profile.as_ref().filter(|_| after_fit) {
            Some(profile) => profile.measure_reacquire(|| self.write_directory()),
            None => self.write_directory(),
        };
        self.waiting_writers.fetch_sub(1, Ordering::Relaxed);
        values
    }
    fn read_directory(&self) -> std::sync::RwLockReadGuard<'_, directory::Directory> {
        io_profile::measured(self.profile.as_ref().map(|p| &p.read_lock_ns), || {
            self.values.read().unwrap()
        })
    }
}

#[no_mangle]
pub unsafe extern "C" fn store_io_times(s: &Store, out: &mut [u64; 4]) {
    *out = s.profile.as_ref().map_or([0; 4], |p| {
        [&p.read_lock_ns, &p.write_lock_ns, &p.pread_ns, &p.pwrite_ns]
            .map(|v| v.load(Ordering::Relaxed))
    });
}

#[no_mangle]
pub unsafe extern "C" fn store_fit_counts(s: &Store, out: &mut [u64; 2]) {
    *out = s.profile.as_ref().map_or([0; 2], |p| {
        [
            p.prepared.load(Ordering::Relaxed),
            p.used.load(Ordering::Relaxed),
        ]
    });
}

#[no_mangle]
pub extern "C" fn store_reacquire_commit_ns(s: &Store) -> u64 {
    s.profile
        .as_ref()
        .map_or(0, |p| p.reacquire_commit_ns.load(Ordering::Relaxed))
}

#[no_mangle]
pub unsafe extern "C" fn store_fit_profile(s: &Store, out: &mut [u64; 7]) {
    *out = s.profile.as_ref().map_or([0; 7], |p| {
        [
            &p.encoding_reuses,
            &p.fit_ns,
            &p.overlap_ns,
            &p.snapshot_ns,
            &p.validation_ns,
            &p.credit_replays,
            &p.reacquire_ns,
        ]
        .map(|counter| counter.load(Ordering::Relaxed))
    });
}

#[no_mangle]
pub unsafe extern "C" fn store_put(s: &Store, page: usize, data: &[u8; PAGE]) -> c_int {
    store_write_pages(s, page, 1, data.as_ptr())
}

/// Commit full pages in address order, releasing the lock at each policy group boundary.
/// A backing error preserves the failing page and leaves the committed prefix visible.
#[no_mangle]
pub unsafe extern "C" fn store_write_pages(
    s: &Store,
    first: usize,
    count: usize,
    data: *const u8,
) -> c_int {
    if first > s.count || count > s.count - first || count > isize::MAX as usize / PAGE {
        return -1;
    }
    if count == 0 {
        return 0;
    }
    if data.is_null() {
        return -1;
    }
    let mut pages = slice::from_raw_parts(data.cast::<[u8; PAGE]>(), count);
    let mut first = first;
    let mut cache = (count > 1).then(literals::FileCandidate::new);
    while !pages.is_empty() {
        let n = pages.len().min(directory::GROUP - first % directory::GROUP);
        let (group, rest) = pages.split_at(n);
        let (mut values, mut leading) = match s.values.try_write() {
            Ok(dir) => (dir, None),
            Err(std::sync::TryLockError::WouldBlock) => {
                // Existing readers permit an immediate snapshot. Behind a writer,
                // queue once so it can release the lock during its next expensive fit.
                let inputs = s
                    .values
                    .try_read()
                    .ok()
                    .and_then(|dir| FitInputs::snapshot(s, &dir, first, &group[0]));
                let leading = inputs.map(|inputs| inputs.prepare(s, &group[0]));
                (s.wait_for_writer(leading.is_some()), leading)
            }
            Err(std::sync::TryLockError::Poisoned(e)) => panic!("{e}"),
        };
        for (i, src) in group.iter().enumerate() {
            let page = first + i;
            let mut prepared = leading.take();
            if prepared.is_none() && s.waiting_writers.load(Ordering::Relaxed) != 0 {
                if let Some(inputs) = FitInputs::snapshot(s, &values, page, src) {
                    drop(values);
                    prepared = Some(inputs.prepare(s, src));
                    values = s.wait_for_writer(true);
                }
            }
            if put_locked(s, &mut values, page, src, prepared, cache.as_mut()) != 0 {
                return -1;
            }
        }
        first += n;
        pages = rest;
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn store_patch(
    s: &Store,
    page: usize,
    offset: usize,
    data: *const u8,
    len: usize,
) -> c_int {
    if page >= s.count || offset > PAGE || len > PAGE - offset || (len != 0 && data.is_null()) {
        return -1;
    }
    let mut values = s.write_directory();
    let old = values.get(page);
    if len == 0 {
        return 0;
    }
    let mut bytes = [0; PAGE];
    if old.decode(&mut bytes).is_err() {
        return -1;
    }
    bytes[offset..offset + len].copy_from_slice(slice::from_raw_parts(data, len));
    put_locked(s, &mut values, page, &bytes, None, None)
}

struct FitInputs {
    selector: Option<Selector>,
    region: search::Region,
}

struct PreparedFit {
    inputs: FitInputs,
    next_region: search::Region,
    encoding: Option<Value>,
    outcome: search::Outcome,
}

fn literal_fast(s: &Store, credits: u64, slot: &Value, data: &[u8; PAGE]) -> bool {
    matches!(
        vram_schedule::run(s.adaptive, credits, false),
        vram_schedule::Adt0::C2
    ) && matches!(slot, Value::Fill(_) | Value::Shared(_))
        && literals::diverse_axes(data)
}

impl FitInputs {
    fn snapshot(
        s: &Store,
        dir: &directory::Directory,
        page: usize,
        data: &[u8; PAGE],
    ) -> Option<Self> {
        io_profile::measured(s.profile.as_ref().map(|p| &p.snapshot_ns), || {
            let slot = dir.get(page);
            let group = dir.group_at(page);
            if data.iter().all(|&b| b == data[0])
                || dir.literals.has_candidate(data, slot.literal_base())
                || literal_fast(s, group.map_or(0, |g| g.literal_credits), slot, data)
            {
                return None;
            }
            Some(Self {
                selector: slot.selector_base().cloned(),
                region: group.map(|g| g.region.clone()).unwrap_or_default(),
            })
        })
    }

    fn prepare(self, s: &Store, data: &[u8; PAGE]) -> PreparedFit {
        let mut next_region = self.region.clone();
        let mut prepare = || {
            let encoding = Value::encode(self.selector.as_ref(), data);
            let outcome = search_page(
                s,
                data,
                encoding.as_ref().map_or(PAGE, Value::charge),
                &mut next_region,
            );
            (encoding, outcome)
        };
        let (encoding, outcome) = if let Some(p) = &s.profile {
            p.measure_fit(prepare)
        } else {
            prepare()
        };
        PreparedFit {
            inputs: self,
            next_region,
            encoding,
            outcome,
        }
    }
}

fn search_page(
    s: &Store,
    data: &[u8; PAGE],
    charge: usize,
    region: &mut search::Region,
) -> search::Outcome {
    let result = search::optimize(data, charge, s.node_budget, s.adaptive.then_some(region));
    // Only observed events need a write to the counters shared by all fitters.
    for (counter, count) in [
        (&s.searches, result.probes),
        (&s.nodes_visited, result.nodes as u64),
        (&s.refits, result.refits),
        (&s.skips, result.skips),
    ] {
        if count != 0 {
            counter.fetch_add(count, Ordering::Relaxed);
        }
    }
    result
}

fn resolve_fit(
    s: &Store,
    prior: Option<&Selector>,
    data: &[u8; PAGE],
    region: &mut search::Region,
    prepared: Option<PreparedFit>,
) -> Option<Value> {
    let encoding = if let Some(p) = prepared {
        let (same_selector, same_region, advance) =
            io_profile::measured(s.profile.as_ref().map(|p| &p.validation_ns), || {
                let same_selector = p.inputs.selector.as_ref() == prior;
                let exact = same_selector && p.inputs.region == *region;
                let advance = same_selector
                    && !exact
                    && region.can_repeat_advance(&p.inputs.region, &p.outcome);
                (same_selector, exact, advance)
            });
        if same_region || advance {
            if advance {
                region.advance();
            } else {
                *region = p.next_region;
            }
            if let Some(profile) = &s.profile {
                profile.used.fetch_add(1, Ordering::Relaxed);
                if advance {
                    profile.credit_replays.fetch_add(1, Ordering::Relaxed);
                }
            }
            return p
                .outcome
                .program
                .map(|program| Value::Program(Box::new(program)))
                .or(p.encoding);
        }
        if same_selector {
            if let Some(profile) = &s.profile {
                profile.encoding_reuses.fetch_add(1, Ordering::Relaxed);
            }
            p.encoding
        } else {
            Value::encode(prior, data)
        }
    } else {
        Value::encode(prior, data)
    };
    search_page(
        s,
        data,
        encoding.as_ref().map_or(PAGE, Value::charge),
        region,
    )
    .program
    .map(|program| Value::Program(Box::new(program)))
    .or(encoding)
}

fn put_locked(
    s: &Store,
    values: &mut directory::Directory,
    page: usize,
    data: &[u8; PAGE],
    prepared: Option<PreparedFit>,
    cache: Option<&mut literals::FileCandidate>,
) -> c_int {
    let _commit_interval = s.profile.as_ref().map(|p| p.begin_commit());
    let dst = values.get(page);
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
    if let Some(shared) = hash.and_then(|h| literals.lookup(h, data, slot.literal_base(), cache)) {
        slot.replace(shared, literals);
        return 0;
    }
    let credits = group.literal_credits;
    let fast = literal_fast(s, credits, slot, data);
    if !fast {
        if let Some(modeled) =
            resolve_fit(s, slot.selector_base(), data, &mut group.region, prepared)
        {
            group.literal_credits = 0;
            slot.replace(modeled, literals);
            return 0;
        }
    }
    let Some(base) = literals.insert(hash, data) else {
        // write_group marks a previously untouched lane before the file I/O.
        if matches!(slot, Value::Fill(0)) {
            values.discard(page, page + 1);
        }
        return -1;
    };
    slot.replace(Value::Shared(base), literals);
    group.literal_credits = vram_advance::run(!fast, credits);
    if fast {
        s.literal_skips.fetch_add(1, Ordering::Relaxed);
    }
    0
}
#[no_mangle]
pub unsafe extern "C" fn store_discard(s: &Store, start: usize, count: usize) -> c_int {
    let mut values = s.values.write().unwrap();
    if start > s.count || count > s.count - start {
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
    let values = s.read_directory();
    if page >= s.count {
        return -1;
    }
    let v = values.get(page);
    if v.decode(data).is_err() {
        return -1;
    }
    if v.selector_base().is_some() {
        s.restores.fetch_add(1, Ordering::Relaxed);
    }
    0
}
// Consecutive slots can be read directly into the first destinations. Expand
// from the end so repeated slots and in-place edits never overwrite a source
// that a lower destination still needs (Performance.compactStep).
fn decode_file_run(
    first: &literal_file::Slot,
    values: &[Value],
    pages: &mut [[u8; PAGE]],
) -> Result<usize, ()> {
    const { assert!(directory::GROUP <= u8::MAX as usize + 1) }
    let mut sources = [0u8; directory::GROUP];
    let mut last_source = 0;
    let mut count = 1;
    for value in &values[1..] {
        let Some(distance) = value
            .literal_base()
            .and_then(|base| base.file_slot())
            .and_then(|slot| slot.distance_from(first))
        else {
            break;
        };
        if distance != last_source && distance != last_source + 1 {
            break;
        }
        sources[count] = distance as u8;
        last_source = distance;
        count += 1;
    }
    first
        .read(pages[..=last_source as usize].as_flattened_mut())
        .map_err(|_| ())?;
    for i in (0..count).rev() {
        let source = usize::from(sources[i]);
        if source < i {
            let (before, after) = pages.split_at_mut(i);
            verdant_tensor::memory::copy_storage_block(&before[source], &mut after[0])
                .map_err(|_| ())?;
        }
        if let Value::Residual(residual) = &values[i] {
            residual.apply(&mut pages[i]);
        }
    }
    Ok(count)
}
// The caller holds a read guard for the entire group. A run can use its first
// destination as the pristine base until every other page has received a copy.
fn decode_group(mut values: &[Value], mut pages: &mut [[u8; PAGE]]) -> Result<u64, ()> {
    let mut restores = 0;
    while let Some((first, rest)) = values.split_first() {
        if rest.is_empty() {
            first.decode(&mut pages[0])?;
            return Ok(restores + u64::from(first.selector_base().is_some()));
        }
        if let Some(slot) = first.literal_base().and_then(|base| base.file_slot()) {
            let decoded = decode_file_run(slot, values, pages)?;
            values = &values[decoded..];
            pages = &mut pages[decoded..];
            continue;
        }
        let (head, tail) = pages.split_first_mut().unwrap();
        if let Some(base) = first.literal_base() {
            base.read(head)?;
            let shared = rest
                .iter()
                .take_while(|next| next.literal_base().is_some_and(|b| Arc::ptr_eq(base, b)));
            let mut copies = 0;
            for (value, dst) in shared.zip(tail.iter_mut()) {
                verdant_tensor::memory::copy_storage_block(head, dst).map_err(|_| ())?;
                if let Value::Residual(residual) = value {
                    residual.apply(dst);
                }
                copies += 1;
            }
            if let Value::Residual(residual) = first {
                residual.apply(head);
            }
            pages = &mut tail[copies..];
            values = &rest[copies..];
        } else {
            first.decode(head)?;
            restores += u64::from(first.selector_base().is_some());
            values = rest;
            pages = tail;
        }
    }
    Ok(restores)
}
/// Read consecutive full pages. An absent group is one contiguous zero range.
#[no_mangle]
pub unsafe extern "C" fn store_read_pages(
    s: &Store,
    first: usize,
    count: usize,
    out: *mut u8,
) -> c_int {
    let values = s.read_directory();
    if first > s.count || count > s.count - first || count > isize::MAX as usize / PAGE {
        return -1;
    }
    if count == 0 {
        return 0;
    }
    if out.is_null() {
        return -1;
    }
    let mut done = 0;
    let mut restores = 0;
    while done < count {
        let page = first + done;
        let lane = page % directory::GROUP;
        let n = (count - done).min(directory::GROUP - lane);
        let dst = out.add(done * PAGE);
        if let Some(group) = values.group_at(page) {
            let pages = slice::from_raw_parts_mut(dst.cast::<[u8; PAGE]>(), n);
            match decode_group(&group.values[lane..lane + n], pages) {
                Ok(decoded) => restores += decoded,
                Err(()) => return -1,
            }
        } else {
            ptr::write_bytes(dst, 0, n * PAGE);
        }
        done += n;
    }
    if restores != 0 {
        s.restores.fetch_add(restores, Ordering::Relaxed);
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
    store_project_rows(s, page, 1, weights, out)
}

#[no_mangle]
pub unsafe extern "C" fn store_project_rows(
    s: &Store,
    page: usize,
    rows: usize,
    weights: *const [i32; SIDE],
    out: *mut [i64; SIDE],
) -> c_int {
    if page >= s.count
        || rows == 0
        || rows > isize::MAX as usize / size_of::<[i64; SIDE]>()
        || weights.is_null()
        || out.is_null()
    {
        return -1;
    }
    let values = s.values.read().unwrap();
    values
        .get(page)
        .project(
            slice::from_raw_parts(weights, rows),
            slice::from_raw_parts_mut(out, rows),
        )
        .map(i32::from)
        .unwrap_or(-1)
}
#[no_mangle]
pub unsafe extern "C" fn store_stats(s: &Store, out: *mut u64) {
    let out = slice::from_raw_parts_mut(out, 5);
    out.fill(0);
    let values = s.values.read().unwrap();
    out[4] = (size_of::<Store>() + values.metadata() + values.literals.charge()) as u64;
    out[0] = s.count as u64;
    for value in values.iter() {
        out[0] -= 1;
        out[4] += value.charge() as u64;
        match value {
            Value::Fill(_) => out[0] += 1,
            Value::Packed(_) => out[1] += 1,
            Value::Shared(_) => out[2] += 1,
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
    fn palette_preserves_the_full_ranking_at_ties() {
        for seed in 0..64 {
            let histogram = std::array::from_fn(|i| (i * 113 + seed * i * i) % (1 + seed));
            let mut ranked: [u8; 256] = std::array::from_fn(|i| i as u8);
            ranked.sort_unstable_by_key(|&i| (std::cmp::Reverse(histogram[usize::from(i)]), i));
            assert_eq!(palette(&histogram), ranked[..16]);
        }
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
        let mut value = Value::encode(None, &page).unwrap();
        assert!(matches!(value, Value::Selector(_)));
        for (index, mask, edits) in [
            (0, 18, 1),
            (4095, 42, 2),
            (0, 7, 2),
            (0, 21, 1),
            (4095, 42, 0),
        ] {
            page[index] ^= mask;
            value = Value::encode(value.selector_base(), &page).unwrap();
            recover(&value, &page);
            if edits == 0 {
                assert!(matches!(value, Value::Selector(_)));
            } else {
                assert!(matches!(&value, Value::Corrected(p) if p.edits.len() == edits));
            }
        }
        assert_eq!(page, original);
        let entropy = std::array::from_fn(|i| (i % 256) as u8);
        assert!(Value::encode(value.selector_base(), &entropy).is_none());
        let rows = std::array::from_fn(|i| (i / SIDE) as u8);
        let value = Value::encode(None, &rows).unwrap();
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

    fn profiled_store(adaptive: bool) -> Box<Store> {
        let mut store =
            unsafe { Box::from_raw(store_new_config(128, search::MAX_NODES, adaptive)) };
        store.profile = Some(Arc::default());
        store
    }

    #[test]
    fn writer_can_finish_fitting_while_reader_holds_directory() {
        let s = profiled_store(true);
        let page = std::array::from_fn(|i| (i / SIDE) as u8);
        let read = s.read_directory();
        std::thread::scope(|scope| {
            let worker = scope.spawn(|| unsafe { store_put(&s, 0, &page) });
            let profile = s.profile.as_ref().unwrap();
            let deadline = std::time::Instant::now() + std::time::Duration::from_secs(10);
            while profile.prepared.load(Ordering::Relaxed) == 0
                && std::time::Instant::now() < deadline
            {
                std::thread::yield_now();
            }
            let fitted = profile.prepared.load(Ordering::Relaxed);
            drop(read);
            assert_eq!(worker.join().unwrap(), 0);
            assert_eq!(fitted, 1, "fit needed the exclusive lock");
            assert_eq!(profile.used.load(Ordering::Relaxed), 1);
        });
        let mut restored = [0; PAGE];
        assert_eq!(unsafe { store_get(&s, 0, &mut restored) }, 0);
        assert_eq!(restored, page);
    }

    #[test]
    fn commit_revalidates_selector_region_and_new_literal() {
        let dense = std::array::from_fn(|i| ((i * 17 + i / 7) % 251) as u8);
        let selector = std::array::from_fn(|i| {
            let row = (i / SIDE) as u8;
            if i % 2 == 0 {
                row
            } else {
                !row
            }
        });
        let mut edited = selector;
        edited[63] ^= 41;
        for (adaptive, competing_page, competing_bytes, submitted) in [
            (false, 0, &selector, &edited), // only the incumbent selector changes
            (true, 1, &dense, &edited),     // only the regional search inputs change
            (true, 64, &dense, &dense),     // exact base appears in another group
        ] {
            let s = profiled_store(adaptive);
            let inputs = FitInputs::snapshot(&s, &s.read_directory(), 0, submitted).unwrap();
            let prepared = inputs.prepare(&s, submitted);
            assert_eq!(unsafe { store_put(&s, competing_page, competing_bytes) }, 0);
            let mut dir = s.write_directory();
            assert_eq!(
                put_locked(&s, &mut dir, 0, submitted, Some(prepared), None),
                0
            );
            assert_eq!(
                s.profile.as_ref().unwrap().used.load(Ordering::Relaxed),
                0,
                "used stale fit after competing write to page {competing_page}"
            );
            assert_eq!(
                s.profile
                    .as_ref()
                    .unwrap()
                    .encoding_reuses
                    .load(Ordering::Relaxed),
                u64::from(competing_page == 1)
            );
            if competing_page == 0 {
                assert!(matches!(dir.get(0), Value::Corrected(_)));
            }
            if competing_page == 64 {
                assert_eq!((dir.literals.live(), dir.literals.hits), (1, 1));
            }
            let mut restored = [0; PAGE];
            dir.get(0).decode(&mut restored).unwrap();
            assert_eq!(&restored, submitted);
        }
    }

    fn compare_candidates(reused: Option<Value>, fresh: Option<Value>, data: &[u8; PAGE]) {
        assert_eq!(
            reused.as_ref().map(std::mem::discriminant),
            fresh.as_ref().map(std::mem::discriminant)
        );
        assert_eq!(
            reused.as_ref().map(Value::charge),
            fresh.as_ref().map(Value::charge)
        );
        if let Some(value) = reused {
            let mut restored = [0; PAGE];
            value.decode(&mut restored).unwrap();
            assert_eq!(&restored, data);
        }
    }

    #[test]
    fn prepared_schedule_matches_fresh_fit_at_every_credit_boundary() {
        let store = profiled_store(true);
        let field = |key: usize| {
            let x = (key as u32).wrapping_mul(0x9e3779b9);
            ((x ^ (x >> 17)).wrapping_mul(0x85ebca6b) >> 24) as u8
        };
        let tiled = std::array::from_fn(|i| {
            let (r, c) = (i / SIDE, i % SIDE);
            field(if r < 32 {
                r * 101 + c % 5
            } else {
                c * 131 + r % 7
            })
        });
        let dense = std::array::from_fn(field);
        let profile = store.profile.as_ref().unwrap();
        // Transposing the field swaps the successful cut directions. A live
        // refit of another nonempty shape must go through the regional search.
        let transposed = std::array::from_fn(|i| tiled[(i % SIDE) * SIDE + i / SIDE]);
        let mut before = search::Region::default();
        let mut live = search::Region::default();
        assert!(search_page(&store, &tiled, PAGE, &mut before)
            .program
            .is_some());
        assert!(search_page(&store, &transposed, PAGE, &mut live)
            .program
            .is_some());
        let cached = FitInputs {
            selector: None,
            region: before,
        }
        .prepare(&store, &tiled);
        assert_eq!(cached.outcome.refits, 1);
        let mut expected = live.clone();
        let reused = resolve_fit(&store, None, &tiled, &mut live, Some(cached));
        let fresh = resolve_fit(&store, None, &tiled, &mut expected, None);
        assert_eq!(profile.encoding_reuses.load(Ordering::Relaxed), 1);
        assert!(live == expected);
        compare_candidates(reused, fresh, &tiled);

        for data in [&tiled, &dense] {
            let mut initial = search::Region::default();
            search_page(&store, data, PAGE, &mut initial);
            let mut live = initial.clone();
            for consumed in 0..=15 {
                let inputs = FitInputs {
                    selector: None,
                    region: initial.clone(),
                };
                let cached = inputs.prepare(&store, data);
                assert_eq!(cached.outcome.refits + cached.outcome.skips, 1);
                let (mut replayed, mut fresh) = (live.clone(), live.clone());
                let before = profile.credit_replays.load(Ordering::Relaxed);
                let reused = resolve_fit(&store, None, data, &mut replayed, Some(cached));
                let expected = resolve_fit(&store, None, data, &mut fresh, None);
                assert!(
                    replayed == fresh,
                    "state diverged after {consumed} neighboring writes"
                );
                assert_eq!(
                    profile.credit_replays.load(Ordering::Relaxed) - before,
                    u64::from((1..15).contains(&consumed))
                );
                compare_candidates(reused, expected, data);
                live = fresh;
            }
        }
    }

    #[test]
    fn full_write_validates_extent_before_committing_and_crosses_groups() {
        let store = unsafe { Box::from_raw(store_new(132)) };
        let src: Vec<[u8; PAGE]> = (1..=69).map(|value| [value; PAGE]).collect();
        let mut bytes = [0; PAGE];
        unsafe {
            assert_eq!(store_write_pages(&store, 63, 70, src.as_ptr().cast()), -1);
            assert_eq!(store_write_pages(&store, 132, 0, ptr::null()), 0);
            assert_eq!(store_write_pages(&store, 63, 1, ptr::null()), -1);
            assert_eq!(store_get(&store, 63, &mut bytes), 0);
            assert_eq!(bytes, [0; PAGE]);
            assert_eq!(
                store_write_pages(&store, 63, src.len(), src.as_ptr().cast()),
                0
            );
            for (i, expected) in src.iter().enumerate() {
                assert_eq!(store_get(&store, 63 + i, &mut bytes), 0);
                assert_eq!(&bytes, expected);
            }
        }
    }

    #[test]
    fn simultaneous_disjoint_patches_survive_group_reads() {
        const COUNT: usize = 257;
        let s = unsafe { Box::from_raw(store_new(COUNT)) };
        let start = std::sync::Barrier::new(4);
        std::thread::scope(|scope| {
            for lane in 0..4 {
                let (s, start) = (&s, &start);
                scope.spawn(move || {
                    let data = [1 + lane as u8; 16];
                    for page in 0..128 {
                        start.wait();
                        let rc =
                            unsafe { store_patch(s, page, lane * 16, data.as_ptr(), data.len()) };
                        assert_eq!(rc, 0);
                    }
                });
            }
        });
        let mut out = vec![99; COUNT * PAGE];
        unsafe {
            assert_eq!(store_put(&s, COUNT - 1, &[77; PAGE]), 0);
            assert_eq!(store_patch(&s, COUNT, 0, ptr::null(), 0), -1);
            assert_eq!(store_patch(&s, 0, PAGE, out.as_ptr(), 1), -1);
            assert_eq!(store_read_pages(&s, COUNT - 1, 2, out.as_mut_ptr()), -1);
            assert_eq!(store_read_pages(&s, COUNT, 0, ptr::null_mut()), 0);
            assert_eq!(store_read_pages(&s, 0, COUNT, out.as_mut_ptr()), 0);
        }
        for (index, page) in out.chunks_exact(PAGE).enumerate() {
            for (i, byte) in page.iter().enumerate() {
                let expected = if index < 128 && i < 64 {
                    1 + (i / 16) as u8
                } else if index == COUNT - 1 {
                    77
                } else {
                    0
                };
                assert_eq!(*byte, expected, "page {index}, byte {i}");
            }
        }
    }

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
    fn readers_restore_whole_pages_during_overwrite_and_trim() {
        use std::sync::Barrier;
        unsafe {
            let store = Box::from_raw(store_new(256));
            let dense = std::array::from_fn(|i| ((i * 17 + i / 7) % 251) as u8);
            let mut edited = dense;
            edited[77] ^= 43;
            let zero = [0; PAGE];
            let start = Barrier::new(2);
            std::thread::scope(|scope| {
                scope.spawn(|| {
                    start.wait();
                    for _ in 0..8 {
                        for page in 0..256 {
                            assert_eq!(store_put(&store, page, &dense), 0);
                            assert_eq!(store_put(&store, page, &edited), 0);
                        }
                        assert_eq!(store_discard(&store, 1, 254), 0);
                        assert_eq!(store_discard(&store, 0, 256), 0);
                    }
                });
                start.wait();
                let mut restored = [0; PAGE];
                for i in 0..4096 {
                    assert_eq!(store_get(&store, (i * 37) % 256, &mut restored), 0);
                    assert!(restored == zero || restored == dense || restored == edited);
                }
            });
            assert_eq!(store.values.read().unwrap().literals.live(), 0);
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
    fn file_bases_outlive_original_addresses_and_reuse_slots() {
        use std::os::fd::AsRawFd;
        use std::os::unix::fs::OpenOptionsExt;
        let path = std::env::temp_dir().join(format!("vram-literal-test-{}", std::process::id()));
        let file = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&path)
            .unwrap();
        std::fs::remove_file(path).unwrap();
        let n = 4;
        file.set_len(((n + 1) * PAGE) as u64).unwrap();
        unsafe {
            assert!(store_new_file(n + 1, file.as_raw_fd()).is_null());
            let store = Box::from_raw(store_new_file(n, file.as_raw_fd()));
            let a = std::array::from_fn(|i| ((i * 17 + i / 7) % 251) as u8);
            let mut edited = a;
            edited[77] ^= 43;
            for (addr, page) in [(0, &a), (1, &a), (2, &edited)] {
                assert_eq!(store_put(&store, addr, page), 0);
            }
            let mut previous = a;
            let mut out = [0; PAGE];
            for round in 1..24u8 {
                let b = a.map(|v| v ^ round);
                assert_eq!(store_put(&store, 0, &b), 0);
                assert_eq!(store_put(&store, 3, &previous), 0);
                for (addr, page) in [(0, &b), (1, &a), (2, &edited), (3, &previous)] {
                    assert_eq!(store_get(&store, addr, &mut out), 0);
                    assert_eq!(&out, page);
                }
                previous = b;
            }
            let (mut reads, mut writes) = (0, 0);
            store_file_stats(&store, &mut reads, &mut writes);
            assert!(reads >= 92 && writes >= 24);
            // A failed read must propagate through the residual restore too.
            file.set_len(0).unwrap();
            assert_eq!(store_get(&store, 2, &mut out), -1);
            assert_eq!(store_discard(&store, 0, n), 0);
            assert_eq!(store.values.read().unwrap().literals.live(), 0);
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
