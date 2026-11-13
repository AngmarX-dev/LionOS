//! x86 port-I/O and CPU primitives for the 32-bit bootstrap.
use core::arch::asm;

#[inline]
pub unsafe fn outb(port: u16, value: u8) {
    asm!("out dx, al", in("dx") port, in("al") value, options(nomem, nostack, preserves_flags));
}
#[inline]
pub unsafe fn inb(port: u16) -> u8 {
    let value: u8;
    asm!("in al, dx", in("dx") port, out("al") value, options(nomem, nostack, preserves_flags));
    value
}
#[inline]
pub unsafe fn outl(port: u16, value: u32) {
    asm!("out dx, eax", in("dx") port, in("eax") value, options(nomem, nostack, preserves_flags));
}
#[inline]
pub unsafe fn inl(port: u16) -> u32 {
    let value: u32;
    asm!("in eax, dx", in("dx") port, out("eax") value, options(nomem, nostack, preserves_flags));
    value
}
#[inline]
pub unsafe fn interrupts_disable() { asm!("cli", options(nomem, nostack, preserves_flags)); }
#[inline]
pub unsafe fn interrupts_enable() { asm!("sti", options(nomem, nostack, preserves_flags)); }
#[inline]
pub unsafe fn halt() { asm!("hlt", options(nomem, nostack, preserves_flags)); }
#[inline]
pub unsafe fn pause() { asm!("pause", options(nomem, nostack, preserves_flags)); }
