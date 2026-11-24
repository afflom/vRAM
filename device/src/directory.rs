// Each occupied bit describes one page in the same 64-page region used by the
// adaptive optimizer. Absent groups represent zero pages, including projections.
use super::{search::Region, Value};
use std::mem::size_of;

const GROUP: usize = 64;
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
    leaves: Vec<Option<Box<Leaf>>>,
}
impl Directory {
    pub(super) fn new(count: usize) -> Option<Self> {
        let mut leaves = Vec::new();
        let n = count.div_ceil(LEAF);
        leaves.try_reserve_exact(n).ok()?;
        leaves.resize_with(n, || None);
        Some(Self {
            count,
            leaves,
            literals: super::literals::Pool::new(count),
        })
    }
    pub(super) fn get(&self, page: usize) -> Option<&Value> {
        if page >= self.count {
            return None;
        }
        Some(
            self.leaves[page / LEAF]
                .as_ref()
                .and_then(|leaf| leaf.groups[page / GROUP % FANOUT].as_ref())
                .map_or(&ZERO, |g| &g.values[page % GROUP]),
        )
    }
    pub(super) fn write_group(&mut self, page: usize) -> &mut Group {
        let leaf = self.leaves[page / LEAF].get_or_insert_with(|| {
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
        group
    }
    pub(super) fn discard(&mut self, mut start: usize, end: usize) {
        while start < end {
            let slot = &mut self.leaves[start / LEAF];
            let Some(leaf) = slot.as_mut() else {
                start += (LEAF - start % LEAF).min(end - start);
                continue;
            };
            let group = &mut leaf.groups[start / GROUP % FANOUT];
            let lane = start % GROUP;
            let n = (GROUP - lane).min(end - start);
            if let Some(g) = group.as_mut() {
                let mask = (u64::MAX >> (GROUP - n)) << lane;
                g.occupied = vram_word::run(g.occupied, 0, mask);
                if vram_empty_group::run(g.occupied) {
                    *group = None;
                    leaf.live -= 1;
                    if leaf.live == 0 {
                        *slot = None;
                    }
                } else {
                    for value in &mut g.values[lane..lane + n] {
                        *value = Value::Fill(0);
                    }
                }
            }
            start += n;
        }
    }
    fn groups(&self) -> impl Iterator<Item = &Group> {
        self.leaves
            .iter()
            .flatten()
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
        self.leaves.capacity() * size_of::<Option<Box<Leaf>>>()
            + self.leaves.iter().flatten().count() * size_of::<Leaf>()
            + self
                .groups()
                .map(|g| size_of::<Group>() + g.region.extra_bytes())
                .sum::<usize>()
    }
}
