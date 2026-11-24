// One immutable slot per distinct file-backed literal. A residual owns its base
// through Arc, so the slot cannot be reused while any decoder can still see it.
use super::PAGE;
use std::fs::File;
use std::io;
use std::os::unix::fs::FileExt;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

// Only words with an available slot are linked. The link is part of the
// word so that releasing a slot can never need a heap allocation.
const NO_WORD: usize = usize::MAX;
struct SlotWord {
    available: u64,
    next: usize,
}

struct Slots {
    words: Vec<SlotWord>,
    head: usize,
    live: u64,
}

impl Default for Slots {
    fn default() -> Self {
        Self {
            words: Vec::new(),
            head: NO_WORD,
            live: 0,
        }
    }
}

impl Slots {
    fn vacant(&self, capacity: u64) -> Option<u64> {
        if self.head == NO_WORD {
            let first = self.words.len() as u64 * 64;
            return (first < capacity).then_some(first);
        }
        let lane = self.words[self.head].available.trailing_zeros();
        Some(self.head as u64 * 64 + u64::from(lane))
    }

    // Called under the same lock as vacant, only after the write succeeds.
    fn claim(&mut self, index: u64, capacity: u64) -> Option<()> {
        let group = (index / 64) as usize;
        if group == self.words.len() {
            self.words.try_reserve(1).ok()?;
            let lanes = (capacity - index).min(64);
            self.words.push(SlotWord {
                available: u64::MAX >> (64 - lanes),
                next: NO_WORD,
            });
            self.head = group;
        }
        let word = &mut self.words[group];
        word.available =
            super::lifecycle::vram_membership(word.available, 1 << (index % 64), false);
        if vram_empty_group::run(word.available) {
            self.head = word.next;
        }
        self.live += 1;
        Some(())
    }

    fn release(&mut self, index: u64) {
        self.live -= 1;
        if self.live == 0 {
            *self = Self::default();
            return;
        }
        let group = (index / 64) as usize;
        let word = &mut self.words[group];
        if vram_empty_group::run(word.available) {
            word.next = self.head;
            self.head = group;
        }
        word.available = super::lifecycle::vram_membership(word.available, 1 << (index % 64), true);
    }
}

pub(super) struct Arena {
    file: File,
    capacity: u64,
    slots: Mutex<Slots>,
    pub(super) reads: AtomicU64,
    pub(super) writes: AtomicU64,
}

pub(super) struct Slot {
    arena: Arc<Arena>,
    index: u64,
}

impl Arena {
    pub(super) fn new(file: File, pages: usize) -> Option<Arc<Self>> {
        // The previous page remains valid until its replacement commits.
        let capacity = (pages as u64).checked_add(1)?;
        let metadata = file.metadata().ok()?;
        if !metadata.is_file() || metadata.len() != capacity.checked_mul(PAGE as u64)? {
            return None;
        }
        Some(Arc::new(Self {
            file,
            capacity,
            slots: Mutex::default(),
            reads: AtomicU64::new(0),
            writes: AtomicU64::new(0),
        }))
    }

    pub(super) fn store(self: &Arc<Self>, bytes: &[u8; PAGE]) -> Option<Slot> {
        let mut slots = self.slots.lock().unwrap();
        let index = slots.vacant(self.capacity)?;
        // A partial write or ledger allocation failure affects only an unowned
        // slot. Its bytes are overwritten in full before a reader can see it.
        self.file.write_all_at(bytes, index * PAGE as u64).ok()?;
        slots.claim(index, self.capacity)?;
        self.writes.fetch_add(1, Ordering::Relaxed);
        Some(Slot {
            arena: self.clone(),
            index,
        })
    }

    pub(super) fn metadata_bytes(&self) -> usize {
        std::mem::size_of::<Self>()
            + self.slots.lock().unwrap().words.capacity() * std::mem::size_of::<SlotWord>()
    }
}

impl Slot {
    pub(super) fn read(&self, bytes: &mut [u8; PAGE]) -> io::Result<()> {
        self.arena
            .file
            .read_exact_at(bytes, self.index * PAGE as u64)?;
        self.arena.reads.fetch_add(1, Ordering::Relaxed);
        Ok(())
    }
}

impl Drop for Slot {
    fn drop(&mut self) {
        self.arena.slots.lock().unwrap().release(self.index);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs::{remove_file, OpenOptions};

    #[test]
    fn fragmented_release_reuses_slots_without_growing_metadata() {
        // The final word has one valid slot. The arena includes the replacement slot.
        const COUNT: usize = 8193;
        let path = std::env::temp_dir().join(format!("vram-slot-test-{}", std::process::id()));
        let file = OpenOptions::new()
            .create_new(true)
            .read(true)
            .write(true)
            .open(&path)
            .unwrap();
        remove_file(path).unwrap();
        file.set_len((COUNT * PAGE) as u64).unwrap();
        let arena = Arena::new(file, COUNT - 1).unwrap();
        let empty = arena.metadata_bytes();
        let mut bytes = [0; PAGE];
        let mut owners: Vec<_> = (0..COUNT)
            .map(|i| {
                bytes[..8].copy_from_slice(&(i as u64).to_le_bytes());
                Some(arena.store(&bytes).unwrap())
            })
            .collect();
        assert!(arena.store(&bytes).is_none());
        let full = arena.metadata_bytes();
        for owner in owners.iter_mut().step_by(2) {
            owner.take();
        }
        let fragmented = arena.metadata_bytes();
        eprintln!("slot ledger bytes: empty={empty}, full={full}, fragmented={fragmented}");
        assert!(fragmented <= full, "slot reclamation grew metadata");
        for (i, owner) in owners
            .iter_mut()
            .enumerate()
            .filter(|(_, slot)| slot.is_none())
        {
            bytes[..8].copy_from_slice(&(!(i as u64)).to_le_bytes());
            *owner = Some(arena.store(&bytes).unwrap());
        }
        assert!(arena.store(&bytes).is_none());
        for (i, owner) in owners.iter().enumerate() {
            let mut actual = [0; PAGE];
            owner.as_ref().unwrap().read(&mut actual).unwrap();
            bytes[..8]
                .copy_from_slice(&(if i % 2 == 0 { !(i as u64) } else { i as u64 }).to_le_bytes());
            assert_eq!(actual, bytes);
        }
        drop(owners);
        assert_eq!(arena.metadata_bytes(), empty);
        assert_eq!(arena.store(&bytes).unwrap().index, 0);
    }
}
