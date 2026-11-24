//! Bounded dynamic programming over dyadic cuts and exact one-hot factorizations.
//! A program owns every byte needed to reconstruct its tile.
use super::{PAGE, SIDE};
use std::{collections::HashMap, mem::size_of};
use verdant_tensor::coded::{
    contract_packed_animation_tracks, decide_uorc_challenger_admission, UorcChallengerVerdict,
};
use vram_search::{run, Adt0 as Request};

pub(super) const MAX_NODES: usize = 49; // (1 + 2 + 4) rectangles along each axis
const RAW: u8 = 0;
const FILL: u8 = 1;
const FACTOR_COLUMNS: u8 = 2;
const FACTOR_ROWS: u8 = 3;
const CUT_ROWS: u8 = 4;
const CUT_COLUMNS: u8 = 5;

#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct Tile {
    x: usize,
    y: usize,
    w: usize,
    h: usize,
}
const ROOT: Tile = Tile {
    x: 0,
    y: 0,
    w: SIDE,
    h: SIDE,
};
impl Tile {
    fn cut(self, rows: bool) -> (Self, Self) {
        if rows {
            let top = Self {
                h: self.h / 2,
                ..self
            };
            (
                top,
                Self {
                    y: self.y + top.h,
                    ..top
                },
            )
        } else {
            let left = Self {
                w: self.w / 2,
                ..self
            };
            (
                left,
                Self {
                    x: self.x + left.w,
                    ..left
                },
            )
        }
    }
    fn index(self, depth: usize, axis: usize, columns: bool) -> usize {
        if columns {
            (self.y + depth) * SIDE + self.x + axis
        } else {
            (self.y + axis) * SIDE + self.x + depth
        }
    }
    fn dims(self, columns: bool) -> (usize, usize) {
        if columns {
            (self.h, self.w)
        } else {
            (self.w, self.h)
        }
    }
}
fn bits(classes: usize) -> usize {
    match classes {
        1 => 0,
        2 => 1,
        3..=4 => 2,
        _ => 4,
    }
}
fn improves(old: usize, new: usize, exact: bool) -> bool {
    run(Request::C2(old as u64, new as u64, exact))
}

struct Search<'a> {
    page: &'a [u8; PAGE],
    cache: HashMap<Tile, Vec<u8>>,
    remaining: usize,
}

impl Search<'_> {
    fn raw(&self, t: Tile) -> Vec<u8> {
        let mut bytes = Vec::with_capacity(1 + t.h * t.w);
        bytes.push(RAW);
        for row in t.y..t.y + t.h {
            let start = row * SIDE + t.x;
            bytes.extend_from_slice(&self.page[start..start + t.w]);
        }
        bytes
    }
    // Partition the observed axis by equality of whole vectors. The partition
    // determines both rank and labels, independent of any workload identifier.
    fn factor(&self, t: Tile, columns: bool, bound: usize) -> Option<Vec<u8>> {
        let (depth, axis) = t.dims(columns);
        let mut representatives = [0; 16];
        let mut classes = 0;
        let mut labels = [0u8; SIDE];
        for (a, label) in labels[..axis].iter_mut().enumerate() {
            let found = representatives[..classes].iter().position(|&b| {
                (0..depth)
                    .all(|d| self.page[t.index(d, a, columns)] == self.page[t.index(d, b, columns)])
            });
            let class = match found {
                Some(class) => class,
                None => {
                    classes += 1;
                    if !run(Request::C3(classes as u64, axis as u64))
                        || 2 + depth * classes + axis * bits(classes) / 8 >= bound
                    {
                        return None;
                    }
                    representatives[classes - 1] = a;
                    classes - 1
                }
            };
            *label = class as u8;
        }
        let width = bits(classes);
        let indices = axis * width / 8;
        let mut code = vec![0; 2 + indices];
        code[0] = if columns { FACTOR_COLUMNS } else { FACTOR_ROWS };
        code[1] = classes as u8;
        if width > 0 {
            for (i, &label) in labels[..axis].iter().enumerate() {
                code[2 + i * width / 8] |= label << (i * width % 8);
            }
        }
        for d in 0..depth {
            for &a in &representatives[..classes] {
                code.push(self.page[t.index(d, a, columns)]);
            }
        }
        Some(code)
    }

    fn best(&mut self, t: Tile) -> Vec<u8> {
        if let Some(found) = self.cache.get(&t) {
            return found.clone();
        }
        let mut best = self.raw(t);
        if self.remaining == 0 {
            return best;
        }
        self.remaining -= 1;
        let first = best[1];
        if best[1..].iter().all(|&b| b == first) {
            best = vec![FILL, first];
        } else {
            for columns in [true, false] {
                if let Some(candidate) = self.factor(t, columns, best.len()) {
                    if improves(best.len(), candidate.len(), true) {
                        best = candidate;
                    }
                }
            }
            for rows in [true, false] {
                let extent = if rows { t.h } else { t.w };
                // Cached subproblems remain available after the last credit is spent.
                let (a, b) = t.cut(rows);
                let available = self.remaining > 0
                    || (self.cache.contains_key(&a) && self.cache.contains_key(&b));
                if !run(Request::C1(extent as u64, u64::from(available))) {
                    continue;
                }
                let left = self.best(a);
                if 1 + left.len() + 2 >= best.len() {
                    continue;
                }
                let right = self.best(b);
                if improves(best.len(), 1 + left.len() + right.len(), true) {
                    best = Vec::with_capacity(1 + left.len() + right.len());
                    best.push(if rows { CUT_ROWS } else { CUT_COLUMNS });
                    best.extend(left);
                    best.extend(right);
                }
            }
        }
        self.cache.insert(t, best.clone());
        best
    }
}

pub(super) struct Program {
    code: Box<[u8]>,
}
impl Program {
    pub(super) fn charge(&self) -> usize {
        size_of::<Self>() + self.code.len()
    }
    pub(super) fn synthesize(
        page: &[u8; PAGE],
        incumbent: usize,
        limit: usize,
    ) -> (Option<Self>, usize) {
        if !run(Request::C0(incumbent as u64, limit as u64)) {
            return (None, 0);
        }
        let mut search = Search {
            page,
            cache: HashMap::new(),
            remaining: limit,
        };
        let program = Self {
            code: search.best(ROOT).into_boxed_slice(),
        };
        (program.admit(page, incumbent), limit - search.remaining)
    }
    fn admit(self, page: &[u8; PAGE], incumbent: usize) -> Option<Self> {
        let program = self;
        if !improves(incumbent, program.charge(), true) {
            return None;
        }
        let mut reference = [0; PAGE];
        let mut fast = [0; PAGE];
        let exact = program.decode(&mut reference, true).is_ok()
            && reference == *page
            && program.decode(&mut fast, false).is_ok()
            && reference == fast;
        if improves(incumbent, program.charge(), exact)
            && matches!(
                decide_uorc_challenger_admission(page, &fast, true, incumbent, program.charge()),
                UorcChallengerVerdict::AdmitChallenger
            )
        {
            Some(program)
        } else {
            None
        }
    }
    pub(super) fn decode(&self, out: &mut [u8; PAGE], reference: bool) -> Result<(), ()> {
        let mut code = &self.code[..];
        decode_tile(&mut code, ROOT, out, reference)?;
        if code.is_empty() {
            Ok(())
        } else {
            Err(())
        }
    }
}

fn take<'a>(code: &mut &'a [u8], len: usize) -> Result<&'a [u8], ()> {
    let (head, rest) = code.split_at_checked(len).ok_or(())?;
    *code = rest;
    Ok(head)
}
fn decode_tile(code: &mut &[u8], t: Tile, out: &mut [u8; PAGE], reference: bool) -> Result<(), ()> {
    let tag = take(code, 1)?[0];
    match tag {
        RAW | FILL => {
            let fill = if tag == FILL {
                Some(take(code, 1)?[0])
            } else {
                None
            };
            for r in t.y..t.y + t.h {
                let row = &mut out[r * SIDE + t.x..r * SIDE + t.x + t.w];
                if let Some(b) = fill {
                    row.fill(b);
                } else {
                    row.copy_from_slice(take(code, t.w)?);
                }
            }
        }
        CUT_ROWS | CUT_COLUMNS => {
            let rows = tag == CUT_ROWS;
            if (if rows { t.h } else { t.w }) <= 16 {
                return Err(());
            }
            let (a, b) = t.cut(rows);
            decode_tile(code, a, out, reference)?;
            decode_tile(code, b, out, reference)?;
        }
        FACTOR_COLUMNS | FACTOR_ROWS => {
            let columns = tag == FACTOR_COLUMNS;
            let (depth, axis) = t.dims(columns);
            let classes = take(code, 1)?[0] as usize;
            if !run(Request::C3(classes as u64, axis as u64)) {
                return Err(());
            }
            let width = bits(classes);
            let packed = take(code, axis * width / 8)?;
            let dict = take(code, depth * classes)?;
            let mut labels = [0usize; SIDE];
            if width > 0 {
                for (i, label) in labels[..axis].iter_mut().enumerate() {
                    *label =
                        ((packed[i * width / 8] >> (i * width % 8)) as usize) & ((1 << width) - 1);
                    if *label >= classes {
                        return Err(());
                    }
                }
            }
            if reference {
                let left: Vec<_> = dict.iter().copied().map(i32::from).collect();
                let mut onehot = vec![0; classes * axis / 2];
                for (i, &label) in labels[..axis].iter().enumerate() {
                    onehot[label * axis / 2 + i / 2] |= 1 << (4 * (i % 2));
                }
                let mut table = [0; 16];
                table[1] = 1;
                let dense =
                    contract_packed_animation_tracks(&left, depth, classes, axis, &table, &onehot)
                        .map_err(|_| ())?;
                for (d, row) in dense.chunks_exact(axis).enumerate() {
                    for (a, &v) in row.iter().enumerate() {
                        out[t.index(d, a, columns)] = v.try_into().map_err(|_| ())?;
                    }
                }
            } else {
                for (group, chunk) in labels[..axis].chunks_exact(8).enumerate() {
                    let mut masks = [0u64; 16];
                    for (lane, &label) in chunk.iter().enumerate() {
                        masks[label] |= 0xffu64 << (8 * lane);
                    }
                    for (d, values) in dict.chunks_exact(classes).enumerate() {
                        let mut word = u64::from_le_bytes([values[0]; 8]);
                        for k in 1..classes {
                            word =
                                vram_word::run(word, u64::from_le_bytes([values[k]; 8]), masks[k]);
                        }
                        let bytes = word.to_le_bytes();
                        let start = t.index(d, group * 8, columns);
                        if columns {
                            out[start..start + 8].copy_from_slice(&bytes);
                        } else {
                            for (i, b) in bytes.into_iter().enumerate() {
                                out[start + i * SIDE] = b;
                            }
                        }
                    }
                }
            }
        }
        _ => return Err(()),
    }
    Ok(())
}

/// The region cache contains only operation tags. Each page's dictionary,
/// ranks, indices and literal bytes are rebuilt from that page's own contents.
#[derive(Clone, Default, PartialEq, Eq)]
pub(super) struct Region {
    shape: Box<[u8]>,
    credits: u64,
}
impl Region {
    pub(super) fn extra_bytes(&self) -> usize {
        self.shape.len()
    }
}
fn describe(code: &mut &[u8], tile: Tile, tags: &mut Vec<u8>) -> Result<(), ()> {
    let tag = take(code, 1)?[0];
    tags.push(tag);
    match tag {
        RAW => {
            take(code, tile.w * tile.h)?;
        }
        FILL => {
            take(code, 1)?;
        }
        FACTOR_COLUMNS | FACTOR_ROWS => {
            let (depth, axis) = tile.dims(tag == FACTOR_COLUMNS);
            let classes = take(code, 1)?[0] as usize;
            if !run(Request::C3(classes as u64, axis as u64)) {
                return Err(());
            }
            take(code, depth * classes + axis * bits(classes) / 8)?;
        }
        CUT_ROWS | CUT_COLUMNS => {
            let (a, b) = tile.cut(tag == CUT_ROWS);
            describe(code, a, tags)?;
            describe(code, b, tags)?;
        }
        _ => return Err(()),
    }
    Ok(())
}
fn refit(search: &mut Search<'_>, tags: &mut &[u8], tile: Tile, out: &mut Vec<u8>) -> Option<()> {
    search.remaining = search.remaining.checked_sub(1)?;
    let tag = take(tags, 1).ok()?[0];
    match tag {
        RAW => out.extend(search.raw(tile)),
        FILL => {
            let b = search.page[tile.index(0, 0, true)];
            if !(0..tile.h).all(|r| (0..tile.w).all(|c| search.page[tile.index(r, c, true)] == b)) {
                return None;
            }
            out.extend([FILL, b]);
        }
        FACTOR_ROWS | FACTOR_COLUMNS => {
            out.extend(search.factor(tile, tag == FACTOR_COLUMNS, 1 + tile.w * tile.h)?)
        }
        CUT_ROWS | CUT_COLUMNS => {
            let (a, b) = tile.cut(tag == CUT_ROWS);
            out.push(tag);
            refit(search, tags, a, out)?;
            refit(search, tags, b, out)?;
        }
        _ => return None,
    }
    Some(())
}

#[derive(Default)]
pub(super) struct Outcome {
    pub program: Option<Program>,
    pub nodes: usize,
    pub probes: u64,
    pub refits: u64,
    pub skips: u64,
}
pub(super) fn optimize(
    page: &[u8; PAGE],
    incumbent: usize,
    limit: usize,
    mut region: Option<&mut Region>,
) -> Outcome {
    use vram_schedule::Adt0 as Job;
    let mut out = Outcome::default();
    if !run(Request::C0(incumbent as u64, limit as u64)) {
        return out;
    }
    let credits = region.as_ref().map_or(0, |r| r.credits);
    let shape = region.as_ref().map_or(&[][..], |r| r.shape.as_ref());
    match vram_schedule::run(region.is_some(), credits, !shape.is_empty()) {
        Job::C2 => {
            if let Some(r) = region {
                r.credits = vram_advance::run(false, credits);
            }
            out.skips = 1;
            return out;
        }
        Job::C1 => {
            let mut search = Search {
                page,
                cache: HashMap::new(),
                remaining: limit,
            };
            let mut tags = shape;
            let mut code = Vec::new();
            if refit(&mut search, &mut tags, ROOT, &mut code).is_some() && tags.is_empty() {
                out.program = Program {
                    code: code.into_boxed_slice(),
                }
                .admit(page, incumbent);
            }
            out.nodes = limit - search.remaining;
            if out.program.is_some() {
                if let Some(r) = region {
                    r.credits = vram_advance::run(false, credits);
                }
                out.refits = 1;
                return out;
            }
        }
        Job::C0 => {}
    }
    let available = limit - out.nodes;
    let (program, visited) = Program::synthesize(page, incumbent, available);
    out.nodes += visited;
    out.probes = u64::from(visited > 0);
    if let Some(r) = region.as_mut() {
        let mut tags = Vec::new();
        if let Some(p) = &program {
            let mut code = &p.code[..];
            if describe(&mut code, ROOT, &mut tags).is_err() || !code.is_empty() {
                tags.clear();
            }
        }
        r.shape = tags.into_boxed_slice();
        r.credits = if visited > 0 {
            vram_advance::run(true, credits)
        } else {
            0
        };
    }
    out.program = program;
    out
}
#[cfg(test)]
mod tests {
    use super::*;

    fn field(seed: u32, i: usize) -> u8 {
        let mut x = (i as u32).wrapping_add(seed).wrapping_mul(0x9e3779b9);
        x ^= x >> 17;
        (x.wrapping_mul(0x85ebca6b) >> 24) as u8
    }
    fn patterned(seed: u32) -> [u8; PAGE] {
        std::array::from_fn(|i| {
            let (r, c) = (i / SIDE, i % SIDE);
            field(
                seed,
                if r < 32 {
                    r * 101 + c % 5
                } else {
                    c * 131 + r % 7
                },
            )
        })
    }
    #[test]
    fn reuse_refresh_and_rediscover_after_distribution_change() {
        let mut region = Region::default();
        let run = |page: &[u8; PAGE], region: &mut Region| {
            let result = optimize(page, PAGE, MAX_NODES, Some(region));
            assert!(result.nodes <= MAX_NODES);
            if let Some(p) = &result.program {
                let mut out = [0; PAGE];
                p.decode(&mut out, false).unwrap();
                assert_eq!(&out, page);
            }
            result
        };
        let first = run(&patterned(1), &mut region);
        assert_eq!(first.probes, 1);
        assert!(first.program.unwrap().charge() < 800);
        let next = run(&patterned(7), &mut region);
        assert_eq!(next.refits, 1);
        assert!(next.nodes < MAX_NODES);
        let random = std::array::from_fn(|i| field(178, i));
        assert!(run(&random, &mut region).program.is_none());
        for _ in 0..15 {
            assert_eq!(run(&patterned(9), &mut region).skips, 1);
        }
        assert!(run(&patterned(9), &mut region).program.is_some());
    }
}
