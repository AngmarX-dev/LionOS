//! Fixed-capacity physical page-frame allocator (4 KiB frames, 32-bit physical space).
use core::ptr::{read_unaligned, write_volatile};

pub const PAGE_SIZE: u32 = 4096;
const FRAME_COUNT: usize = 1_048_576;
const WORD_COUNT: usize = FRAME_COUNT / 32;
static mut USED: [u32; WORD_COUNT] = [u32::MAX; WORD_COUNT];
static mut READY: bool = false;
static mut FREE_FRAMES: u32 = 0;

#[derive(Clone, Copy, Debug)]
pub struct MemoryStats { pub total_usable_bytes: u64, pub free_frames: u32, pub initialized: bool }

unsafe fn set_used(frame: usize, used: bool) {
    let word = frame / 32;
    let mask = 1u32 << (frame % 32);
    if used { USED[word] |= mask; } else { USED[word] &= !mask; }
}

unsafe fn mark_region(base: u64, length: u64, used: bool) {
    if length == 0 { return; }
    let end = base.saturating_add(length).min(0x1_0000_0000);
    let start_frame = ((base.saturating_add(PAGE_SIZE as u64 - 1)) / PAGE_SIZE as u64) as usize;
    let end_frame = (end / PAGE_SIZE as u64) as usize;
    for frame in start_frame.min(FRAME_COUNT)..end_frame.min(FRAME_COUNT) {
        let was_used = (USED[frame / 32] & (1 << (frame % 32))) != 0;
        if was_used && !used { FREE_FRAMES = FREE_FRAMES.saturating_add(1); }
        if !was_used && used { FREE_FRAMES = FREE_FRAMES.saturating_sub(1); }
        set_used(frame, used);
    }
}

/// Initialize from the Multiboot2 memory-map tag. The info pointer must be a
/// validated, identity-mapped Multiboot2 structure supplied by the bootloader.
pub unsafe fn init(multiboot_info: *const u8, total_size: usize) -> bool {
    if multiboot_info.is_null() || total_size < 16 { return false; }
    for word in 0..WORD_COUNT { USED[word] = u32::MAX; }
    FREE_FRAMES = 0;
    let mut offset = 8usize;
    let mut map_seen = false;
    while offset.checked_add(8).map_or(false, |v| v <= total_size) {
        let tag = multiboot_info.add(offset);
        let kind = read_unaligned(tag.cast::<u32>());
        let size = read_unaligned(tag.add(4).cast::<u32>()) as usize;
        if size < 8 || offset.checked_add(size).map_or(true, |v| v > total_size) { break; }
        if kind == 0 { break; }
        if kind == 6 && size >= 16 {
            map_seen = true;
            let entry_size = read_unaligned(tag.add(8).cast::<u32>()) as usize;
            if entry_size < 24 { return false; }
            let mut pos = 16usize;
            while pos.checked_add(entry_size).map_or(false, |v| v <= size) {
                let entry = tag.add(pos);
                let base = read_unaligned(entry.cast::<u64>());
                let len = read_unaligned(entry.add(8).cast::<u64>());
                let typ = read_unaligned(entry.add(16).cast::<u32>());
                if typ == 1 { mark_region(base, len, false); }
                pos += entry_size;
            }
        }
        offset = (offset + size + 7) & !7;
    }
    if !map_seen { return false; }
    // Keep low memory reserved for firmware data, boot structures and the kernel.
    mark_region(0, 0x10_0000, true);
    READY = true;
    true
}

pub fn alloc_frame() -> Option<u32> {
    unsafe {
        if !READY { return None; }
        for word_index in 0..WORD_COUNT {
            let free = !USED[word_index];
            if free != 0 {
                let bit = free.trailing_zeros() as usize;
                let frame = word_index * 32 + bit;
                if frame >= FRAME_COUNT { return None; }
                set_used(frame, true);
                FREE_FRAMES = FREE_FRAMES.saturating_sub(1);
                return Some((frame as u32) * PAGE_SIZE);
            }
        }
        None
    }
}

pub unsafe fn free_frame(address: u32) -> bool {
    if !READY || address % PAGE_SIZE != 0 { return false; }
    let frame = (address / PAGE_SIZE) as usize;
    if frame >= FRAME_COUNT || frame < (0x10_0000 / PAGE_SIZE) as usize { return false; }
    let used = (USED[frame / 32] & (1 << (frame % 32))) != 0;
    if !used { return false; }
    set_used(frame, false);
    FREE_FRAMES = FREE_FRAMES.saturating_add(1);
    true
}

pub fn stats(total_usable_bytes: u64) -> MemoryStats {
    unsafe { MemoryStats { total_usable_bytes, free_frames: FREE_FRAMES, initialized: READY } }
}

#[allow(dead_code)]
unsafe fn volatile_zero(address: *mut u8, length: usize) {
    for i in 0..length { write_volatile(address.add(i), 0); }
}
