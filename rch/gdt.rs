//! Flat 32-bit kernel GDT. Assumes the Multiboot2 loader entered protected mode.
use core::arch::asm;
#[repr(C, packed)]
struct DescriptorPointer { limit: u16, base: u32 }
static mut GDT: [u64; 3] = [
    0,
    0x00CF_9A00_0000_FFFF, // ring-0, 32-bit code, 4 KiB granularity
    0x00CF_9200_0000_FFFF, // ring-0, 32-bit data, 4 KiB granularity
];
pub unsafe fn init() {
    let pointer = DescriptorPointer {
        limit: (core::mem::size_of::<[u64; 3]>() - 1) as u16,
        base: core::ptr::addr_of!(GDT) as u32,
    };
    asm!("lgdt [{0}]", in(reg) &pointer, options(readonly, nostack, preserves_flags));
    asm!(
        "push 0x08",
        "lea eax, [2f]",
        "push eax",
        "retf",
        "2:",
        "mov ax, 0x10",
        "mov ds, ax",
        "mov es, ax",
        "mov fs, ax",
        "mov gs, ax",
        "mov ss, ax",
        out("eax") _,
    );
}
