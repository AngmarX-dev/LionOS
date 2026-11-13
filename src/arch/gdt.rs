//! Flat 32-bit kernel GDT with an available 32-bit TSS descriptor.
use core::arch::asm;
#[repr(C, packed)]
struct DescriptorPointer { limit: u16, base: u32 }
static mut GDT: [u64; 4] = [
    0,
    0x00CF_9A00_0000_FFFF,
    0x00CF_9200_0000_FFFF,
    0,
];

/// Write a byte-granular, present, available 32-bit TSS descriptor at selector 0x18.
pub unsafe fn install_tss_descriptor(base: u32, limit: u32) {
    let descriptor = ((limit & 0xFFFF) as u64)
        | (((base & 0xFFFF) as u64) << 16)
        | ((((base >> 16) & 0xFF) as u64) << 32)
        | (0x89u64 << 40)
        | ((((limit >> 16) & 0x0F) as u64) << 48)
        | ((((base >> 24) & 0xFF) as u64) << 56);
    core::ptr::write_volatile(core::ptr::addr_of_mut!(GDT[3]), descriptor);
}
pub unsafe fn init() {
    let pointer = DescriptorPointer {
        limit: (core::mem::size_of::<[u64; 4]>() - 1) as u16,
        base: core::ptr::addr_of!(GDT) as u32,
    };
    asm!("lgdt [{0}]", in(reg) &pointer, options(readonly, nostack, preserves_flags));
    asm!(
        "push 0x08", "lea eax, [2f]", "push eax", "retf", "2:",
        "mov ax, 0x10", "mov ds, ax", "mov es, ax", "mov fs, ax",
        "mov gs, ax", "mov ss, ax", out("eax") _,
    );
    let stack: u32;
    asm!("mov {0:e}, esp", out(reg) stack, options(nomem, nostack, preserves_flags));
    crate::arch::tss::init(stack);
}
