//! Minimal PS/2 keyboard polling primitive. Requires controller initialization by platform code.
use crate::arch::x86::inb;
pub fn poll_scancode() -> Option<u8> {
    unsafe { if inb(0x64) & 1 != 0 { Some(inb(0x60)) } else { None } }
}
