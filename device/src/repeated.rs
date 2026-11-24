//! Exact reconstruction from an owned prefix and a verified byte displacement.
use super::{PAGE, SIDE};
use std::mem::size_of;
use verdant_tensor::coded::{decide_uorc_challenger_admission, UorcChallengerVerdict};

pub(super) struct Repeated {
    block: Box<[u8]>,
}

impl Repeated {
    /// Equality of both halves permits one fold; a mismatch ends the search.
    pub(super) fn factor(page: &[u8; PAGE]) -> Option<&[u8]> {
        let mut block = &page[..];
        while block.len() > 1 {
            let (first, second) = block.split_at(block.len() / 2);
            if decide_uorc_challenger_admission(first, second, true, block.len(), first.len())
                != UorcChallengerVerdict::AdmitChallenger
            {
                break;
            }
            block = first;
        }
        (block.len() < PAGE).then_some(block)
    }

    pub(super) fn last_displacement(incumbent: usize) -> usize {
        incumbent.saturating_sub(Self::cost(0) + 1).min(PAGE / 2)
    }

    /// Validate at most one overlap in this range of profitable displacements.
    pub(super) fn shifted(page: &[u8; PAGE], incumbent: usize, first: usize) -> Option<&[u8]> {
        let max = Self::last_displacement(incumbent);
        if first > max {
            return None;
        }
        let prefix = &page[..8];
        let displacement = first + page[first..max + 8].windows(8).position(|w| w == prefix)?;
        let verdict = decide_uorc_challenger_admission(
            &page[displacement..],
            &page[..PAGE - displacement],
            true,
            incumbent,
            Self::cost(displacement),
        );
        (verdict == UorcChallengerVerdict::AdmitChallenger).then_some(&page[..displacement])
    }

    pub(super) fn new(block: &[u8]) -> Box<Self> {
        Box::new(Self {
            block: block.into(),
        })
    }

    pub(super) fn cost(bytes: usize) -> usize {
        size_of::<Self>() + bytes
    }

    pub(super) fn charge(&self) -> usize {
        Self::cost(self.block.len())
    }

    pub(super) fn decode(&self, out: &mut [u8; PAGE]) {
        let mut n = self.block.len();
        out[..n].copy_from_slice(&self.block);
        while n < PAGE {
            let take = n.min(PAGE - n);
            out.copy_within(..take, n);
            n += take;
        }
    }

    pub(super) fn project(&self, weights: &[i32; SIDE], sums: &mut [i64; SIDE]) {
        if !self.block.len().is_power_of_two() {
            // Adjacent rows need not start at the same phase for a general displacement.
            sums.fill(0);
            let mut phase = 0;
            for &weight in weights {
                for sum in &mut *sums {
                    *sum += i64::from(weight) * i64::from(self.block[phase]);
                    phase += 1;
                    if phase == self.block.len() {
                        phase = 0;
                    }
                }
            }
            return;
        }
        let height = self.block.len().div_ceil(SIDE);
        sums.fill(0);
        for r in 0..height {
            let weight: i64 = weights[r..]
                .iter()
                .step_by(height)
                .map(|&w| i64::from(w))
                .sum();
            for (col, sum) in sums.iter_mut().enumerate() {
                *sum += weight * i64::from(self.block[(r * SIDE + col) % self.block.len()]);
            }
        }
    }
}
