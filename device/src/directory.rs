// Each occupied bit describes one page in the same 64-page region used by the
// adaptive optimizer. Absent groups represent zero pages, including projections.
use super::{search::Region, Value};
use std::{collections::BTreeMap, mem::size_of};

pub(super) const GROUP: usize = 64;
const FANOUT: usize = 256;
const LEAF: usize = GROUP * FANOUT;
static ZERO: Value = Value::Fill(0);

pub(super) struct Group {
    pub(super) values: [Value; GROUP],
    pub(super) region: Region,
    pub(super) literal_credits: u64,
    occupied: u64,
}
struct Leaf {
    groups: [Option<Box<Group>>; FANOUT],
    live: usize,
}
pub(super) struct Directory {
    pub(super) count: usize,
    pub(super) literals: super::literals::Pool,
    leaves: BTreeMap<usize, Box<Leaf>>,
}
impl Directory {
    pub(super) fn new(count: usize) -> Self {
        Self {
            count,
            leaves: BTreeMap::new(),
            literals: super::literals::Pool::new(),
        }
    }
    pub(super) fn get(&self, page: usize) -> Option<&Value> {
        if page >= self.count {
            return None;
        }
        Some(
            self.leaves
                .get(&(page / LEAF))
                .and_then(|leaf| leaf.groups[page / GROUP % FANOUT].as_ref())
                .map_or(&ZERO, |g| &g.values[page % GROUP]),
        )
    }
    pub(super) fn write_group(&mut self, page: usize) -> (&mut Group, &mut super::literals::Pool) {
        let leaf = self.leaves.entry(page / LEAF).or_insert_with(|| {
            Box::new(Leaf {
                groups: std::array::from_fn(|_| None),
                live: 0,
            })
        });
        let group = leaf.groups[page / GROUP % FANOUT].get_or_insert_with(|| {
            leaf.live += 1;
            Box::new(Group {
                values: std::array::from_fn(|_| Value::Fill(0)),
                region: Region::default(),
                literal_credits: 0,
                occupied: 0,
            })
        });
        group.occupied = vram_word::run(group.occupied, u64::MAX, 1 << (page % GROUP));
        (group, &mut self.literals)
    }
    pub(super) fn discard(&mut self, start: usize, end: usize) {
        if start == end {
            return;
        }
        let range = start / LEAF..=(end - 1) / LEAF;
        self.leaves
            .extract_if(range, |&id, leaf| {
                let base = id * LEAF;
                let lo = start.saturating_sub(base);
                let hi = (end - base).min(LEAF);
                if lo == 0 && hi == LEAF {
                    return true;
                }
                for index in lo / GROUP..hi.div_ceil(GROUP) {
                    let slot = &mut leaf.groups[index];
                    let Some(g) = slot else { continue };
                    let from = lo.saturating_sub(index * GROUP);
                    let to = (hi - index * GROUP).min(GROUP);
                    let mask = (u64::MAX >> (GROUP - (to - from))) << from;
                    g.occupied = vram_word::run(g.occupied, 0, mask);
                    if vram_empty_group::run(g.occupied) {
                        *slot = None;
                        leaf.live -= 1;
                    } else {
                        for value in &mut g.values[from..to] {
                            *value = Value::Fill(0);
                        }
                    }
                }
                leaf.live == 0
            })
            .for_each(drop);
        self.literals.resize_for_live_bases();
    }
    fn groups(&self) -> impl Iterator<Item = &Group> {
        self.leaves
            .values()
            .flat_map(|leaf| leaf.groups.iter().flatten().map(Box::as_ref))
    }
    pub(super) fn iter(&self) -> impl Iterator<Item = &Value> {
        self.groups().flat_map(|g| {
            g.values
                .iter()
                .enumerate()
                .filter(|(i, _)| g.occupied & (1u64 << i) != 0)
                .map(|(_, value)| value)
        })
    }
    pub(super) fn metadata(&self) -> usize {
        // Charge the live keys, pointers and leaf payloads. Allocator and B-tree
        // node slack are included in process RSS, outside this logical charge.
        self.leaves.len() * (size_of::<(usize, Box<Leaf>)>() + size_of::<Leaf>())
            + self
                .groups()
                .map(|g| size_of::<Group>() + g.region.extra_bytes())
                .sum::<usize>()
    }
}
