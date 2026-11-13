//! PS/2 keyboard polling. Auxiliary-port bytes are reserved for the IRQ12 mouse driver.
use crate::arch::x86::inb;

/// Read one keyboard byte without stealing a pending auxiliary-device packet byte.
pub fn poll_scancode() -> Option<u8> {
    unsafe {
        let status = inb(0x64);
        if status & 0x01 != 0 && status & 0x20 == 0 {
            Some(inb(0x60))
        } else {
            None
        }
    }
}
