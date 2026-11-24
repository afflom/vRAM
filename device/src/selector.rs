// A 64x64 byte page whose rows each select two row-specific values with
// a shared column mask. The two factors are sufficient for reconstruction.
use super::{PAGE, SIDE};
use verdant_tensor::coded::{
    contract_packed_animation_tracks, decide_uorc_challenger_admission, UorcChallengerVerdict,
};
use vram_selector_admission::run as matches_cell;

#[derive(Clone)]
pub(super) struct Selector {
    values: [[u8; 2]; SIDE],
    columns: u64,
}

impl Selector {
    pub(super) fn fit(bytes: &[u8; PAGE]) -> Option<Box<Self>> {
        // A nonconstant row determines the selector up to exchanging its lanes.
        let mut columns = 0u64;
        let mut pivot = 0;
        if let Some(row) = bytes
            .chunks_exact(SIDE)
            .find(|row| row.iter().any(|&b| b != row[0]))
        {
            pivot = row.iter().position(|&b| b != row[0])?;
            let second = row[pivot];
            for (j, &b) in row.iter().enumerate() {
                if b == second {
                    columns |= 1u64 << j;
                } else if b != row[0] {
                    return None;
                }
            }
        }
        let mut values = [[0u8; 2]; SIDE];
        for (v, row) in values.iter_mut().zip(bytes.chunks_exact(SIDE)) {
            *v = [row[0], row[pivot]];
            if row.iter().enumerate().any(|(j, &b)| {
                !matches_cell((columns >> j) & 1 != 0, v[0].into(), v[1].into(), b.into())
            }) {
                return None;
            }
        }
        let model = Self { values, columns };
        let mut reconstructed = [0u8; PAGE];
        model.reference_decode(&mut reconstructed).ok()?;
        let mut specialized = [0; PAGE];
        model.decode(&mut specialized).ok()?;
        if reconstructed != specialized {
            return None;
        }
        let verdict = decide_uorc_challenger_admission(
            bytes,
            &reconstructed,
            true,
            PAGE,
            std::mem::size_of::<Self>(),
        );
        (verdict == UorcChallengerVerdict::AdmitChallenger).then(|| Box::new(model))
    }

    pub(super) fn decode(&self, page: &mut [u8; PAGE]) -> Result<(), ()> {
        let lut = mask_table().ok_or(())?;
        let masks = self.columns.to_le_bytes().map(|b| lut[usize::from(b)]);
        for (row, [a, b]) in page.chunks_exact_mut(SIDE).zip(self.values) {
            let left = u64::from_le_bytes([a; 8]);
            let right = u64::from_le_bytes([b; 8]);
            for (word, mask) in row.chunks_exact_mut(8).zip(masks) {
                word.copy_from_slice(&vram_word::run(left, right, mask).to_le_bytes());
            }
        }
        Ok(())
    }

    fn reference_decode(&self, page: &mut [u8; PAGE]) -> Result<(), ()> {
        let left: [i32; SIDE * 2] = std::array::from_fn(|i| self.values[i / 2][i % 2].into());
        let mut right = [0u8; SIDE]; // two packed 1x64 rows
        for j in 0..SIDE {
            let lane = ((self.columns >> j) & 1) as usize;
            right[lane * (SIDE / 2) + j / 2] |= 1 << (4 * (j % 2));
        }
        let mut table = [0i32; 16];
        table[1] = 1;
        let result = contract_packed_animation_tracks(&left, SIDE, 2, SIDE, &table, &right)
            .map_err(|_| ())?;
        for (out, value) in page.iter_mut().zip(result) {
            *out = u8::try_from(value).map_err(|_| ())?;
        }
        Ok(())
    }
}

// Evaluate the release's generated field-extraction root for all eight lanes.
pub(super) fn mask_table() -> Option<&'static [u64; 256]> {
    static TABLE: std::sync::OnceLock<Option<[u64; 256]>> = std::sync::OnceLock::new();
    TABLE
        .get_or_init(|| {
            let mut table = [0; 256];
            for (i, mask) in table.iter_mut().enumerate() {
                let mut bytes = [0; 8];
                for (j, byte) in bytes.iter_mut().enumerate() {
                    let bit = verdant_tensor::linear::extract_packed_field(i as u64, 1, 1 << j, 2)
                        .ok()??;
                    *byte = match bit {
                        0 => 0,
                        1 => 255,
                        _ => return None,
                    };
                }
                *mask = u64::from_le_bytes(bytes);
            }
            Some(table)
        })
        .as_ref()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reversed_lanes_and_constant_rows_round_trip() {
        let bytes: [u8; PAGE] = std::array::from_fn(|i| {
            let r = (i / SIDE) as u8;
            let c = i % SIDE;
            if r < 2 || c % 7 == 0 {
                r
            } else {
                255 - r
            }
        });
        let model = Selector::fit(&bytes).expect("representable page");
        let mut out = [0; PAGE];
        model.decode(&mut out).unwrap();
        assert_eq!(out, bytes);
        let mut changed = bytes;
        changed[PAGE - 1] ^= 128;
        assert!(Selector::fit(&changed).is_none());
    }
}
