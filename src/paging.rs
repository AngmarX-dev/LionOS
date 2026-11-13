//! Initial 32-bit PSE identity map. This is a bring-up map, not process isolation.
use core::arch::asm;
use core::ptr::write_volatile;

#[repr(align(4096))]
struct PageDirectory([u32;1024]);
static mut DIRECTORY:PageDirectory=PageDirectory([0;1024]);

/// Enable a supervisor-only, writable 4 GiB identity map using 4 MiB pages.
/// Requires an x86 CPU with CR4.PSE support (the declared i686 baseline).
pub unsafe fn init_identity_4g()->bool{
    // Never set CR4.PSE unless the processor advertises 4 MiB page support.
    if !crate::arch::cpu::features().pse { return false; }
    let directory=core::ptr::addr_of_mut!(DIRECTORY.0).cast::<u32>();
    for index in 0..1024usize{
        let entry=((index as u32)<<22)|0x083; // present | writable | 4 MiB page
        write_volatile(directory.add(index),entry);
    }
    let physical=directory as u32;
    asm!(
        "mov eax, cr4",
        "or eax, 0x10",
        "mov cr4, eax",
        "mov cr3, ecx",
        "mov eax, cr0",
        "or eax, 0x80000000",
        "mov cr0, eax",
        "jmp 2f",
        "2:",
        in("ecx") physical,
        out("eax") _,
        options(nostack),
    );
    true
}
