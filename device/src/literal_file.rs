// One immutable slot per distinct file-backed literal. A residual owns its base
// through Arc, so the slot cannot be reused while any decoder can still see it.
use super::PAGE;
use std::fs::File;
use std::io;
use std::os::unix::fs::FileExt;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

#[derive(Default)]
struct Slots {
    next: u64,
    free: Vec<u64>,
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
        let index = if let Some(i) = slots.free.pop() {
            i
        } else {
            if slots.next == self.capacity {
                return None;
            }
            slots.next += 1;
            slots.next - 1
        };
        drop(slots);
        let slot = Slot {
            arena: self.clone(),
            index,
        };
        self.file.write_all_at(bytes, index * PAGE as u64).ok()?;
        self.writes.fetch_add(1, Ordering::Relaxed);
        Some(slot)
    }

    pub(super) fn metadata_bytes(&self) -> usize {
        std::mem::size_of::<Self>()
            + self.slots.lock().unwrap().free.capacity() * std::mem::size_of::<u64>()
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
        let mut slots = self.arena.slots.lock().unwrap();
        if slots.free.len() as u64 + 1 == slots.next {
            // A drained store retains its disk reservation but releases the free list.
            *slots = Slots::default();
        } else if self.index + 1 == slots.next {
            slots.next -= 1;
        } else {
            slots.free.push(self.index);
        }
    }
}
