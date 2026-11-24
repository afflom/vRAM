// C calls the generated state machine through a stable scalar ABI. Event and
// action constructor order is part of the pinned production bundle.
use vram_lifecycle::{run, Adt0 as Event, Adt1 as Action};

fn action(event: Event) -> u32 {
    match run(event) {
        Action::C0 => 0, // skip
        Action::C1 => 1, // install clean and protected
        Action::C2 => 2, // install dirty
        Action::C3 => 3, // allow write
        Action::C4 => 4, // capture
        Action::C5 => 5, // discard
        Action::C6 => 6, // release frame
        Action::C7 => 7, // retain
        Action::C8 => 8, // trim
        Action::C9 => 9, // allow
    }
}

#[no_mangle]
pub extern "C" fn vram_fault(wp: bool, write: bool, tracking: bool) -> u32 {
    action(Event::C0(wp, write, tracking))
}
#[no_mangle]
pub extern "C" fn vram_reclaim(present: bool, dirty: bool) -> u32 {
    action(Event::C1(present, dirty))
}
#[no_mangle]
pub extern "C" fn vram_captured(ok: bool) -> u32 {
    action(Event::C2(ok))
}
#[no_mangle]
pub extern "C" fn vram_discarded(ok: bool) -> u32 {
    action(Event::C3(ok))
}
#[no_mangle]
pub extern "C" fn vram_trim(limit: u64, resident: u64) -> bool {
    matches!(run(Event::C4(limit, resident)), Action::C8)
}
#[no_mangle]
pub extern "C" fn vram_permitted(
    opted: bool,
    quiescent: bool,
    tcg: bool,
    kvm: bool,
    capture_all: bool,
    migration_idle: bool,
) -> bool {
    matches!(
        run(Event::C5(
            opted,
            quiescent,
            tcg,
            kvm,
            capture_all,
            migration_idle
        )),
        Action::C9
    )
}
#[no_mangle]
pub extern "C" fn vram_range(offset: u64, length: u64, extent: u64) -> bool {
    vram_range_model::run(offset, length, extent)
}

#[no_mangle]
pub extern "C" fn vram_membership(word: u64, bit: u64, present: bool) -> u64 {
    vram_word::run(word, if present { u64::MAX } else { 0 }, bit)
}

#[no_mangle]
pub extern "C" fn vram_block_begin(
    framed: bool,
    command: u16,
    offset: u64,
    length: u32,
    extent: u64,
) -> u32 {
    let allowed = vram_range_model::run(offset, u64::from(length), extent);
    vram_block_step::run(0, u64::from(command), allowed, length == 0, framed, false) as u32
}

#[no_mangle]
pub extern "C" fn vram_block_after(effect: u32, success: bool, last: bool) -> u32 {
    vram_block_step::run(u64::from(effect), 0, false, false, success, last) as u32
}
