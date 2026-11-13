//! Interrupt-driven PS/2 keyboard input (set-1 compatible scan-code stream).
use core::sync::atomic::{AtomicU16, AtomicU8, AtomicU32, Ordering};
use crate::arch::x86::inb;

const QUEUE_CAPACITY: u8 = 64;
const QUEUE_MASK: u8 = QUEUE_CAPACITY - 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct KeyEvent {
    /// Raw set-1 scan code without the release bit.
    pub scancode: u8,
    pub pressed: bool,
    pub extended: bool,
}

static mut EVENTS: [AtomicU16; QUEUE_CAPACITY as usize] =
    [const { AtomicU16::new(0) }; QUEUE_CAPACITY as usize];
static HEAD: AtomicU8 = AtomicU8::new(0);
static TAIL: AtomicU8 = AtomicU8::new(0);
static DROPPED: AtomicU32 = AtomicU32::new(0);
static mut EXTENDED_PREFIX: bool = false;
static mut PAUSE_BYTES_LEFT: u8 = 0;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum KeyboardInitError { ControllerTimeout, AuxiliaryDataPending }

const WAIT_LIMIT: usize = 100_000;

unsafe fn wait_input_empty() -> Result<(), KeyboardInitError> {
    for _ in 0..WAIT_LIMIT {
        if inb(0x64) & 0x02 == 0 { return Ok(()); }
        crate::arch::x86::pause();
    }
    Err(KeyboardInitError::ControllerTimeout)
}

/// Enable the keyboard port and its 8042 IRQ1 source, then unmask PIC IRQ1.
/// Call during boot while IF is still clear and after auxiliary mouse setup.
pub unsafe fn initialize_irq() -> Result<(), KeyboardInitError> {
    wait_input_empty()?;
    crate::arch::x86::outb(0x64, 0xAE); // enable first PS/2 port
    wait_input_empty()?;

    crate::arch::x86::outb(0x64, 0x20); // read controller configuration byte
    let mut status = 0u8;
    let mut config = None;
    for _ in 0..WAIT_LIMIT {
        status = inb(0x64);
        if status & 0x01 != 0 {
            if status & 0x20 != 0 { return Err(KeyboardInitError::AuxiliaryDataPending); }
            config = Some(inb(0x60));
            break;
        }
        crate::arch::x86::pause();
    }
    let mut config = config.ok_or(KeyboardInitError::ControllerTimeout)?;
    config |= 0x01; // enable keyboard interrupt
    config &= !0x10; // ensure keyboard clock is enabled
    wait_input_empty()?;
    crate::arch::x86::outb(0x64, 0x60);
    wait_input_empty()?;
    crate::arch::x86::outb(0x60, config);

    let mask = inb(0x21) & !(1 << 1);
    crate::arch::x86::outb(0x21, mask);
    Ok(())
}

/// Called only by IRQ1 after the dispatcher has read port 0x60.
pub fn handle_irq_byte(byte: u8) {
    unsafe {
        if PAUSE_BYTES_LEFT != 0 {
            PAUSE_BYTES_LEFT -= 1;
            return;
        }
        if byte == 0xE1 {
            EXTENDED_PREFIX = false;
            PAUSE_BYTES_LEFT = 5;
            return;
        }
        if byte == 0xE0 {
            EXTENDED_PREFIX = true;
            return;
        }
        let extended = EXTENDED_PREFIX;
        EXTENDED_PREFIX = false;
        let event = (byte & 0x7F) as u16
            | (((byte & 0x80 == 0) as u16) << 7)
            | ((extended as u16) << 8);
        let head = HEAD.load(Ordering::Relaxed);
        let next = head.wrapping_add(1) & QUEUE_MASK;
        if next == TAIL.load(Ordering::Acquire) {
            DROPPED.fetch_add(1, Ordering::Relaxed);
            return;
        }
        (*core::ptr::addr_of_mut!(EVENTS).cast::<AtomicU16>().add(head as usize)).store(event, Ordering::Relaxed);
        HEAD.store(next, Ordering::Release);
    }
}

/// Pop one complete key transition from the single-producer/single-consumer ring.
pub fn pop_event() -> Option<KeyEvent> {
    let tail = TAIL.load(Ordering::Relaxed);
    if tail == HEAD.load(Ordering::Acquire) { return None; }
    let encoded = unsafe { (*core::ptr::addr_of!(EVENTS).cast::<AtomicU16>().add(tail as usize)).load(Ordering::Relaxed) };
    TAIL.store(tail.wrapping_add(1) & QUEUE_MASK, Ordering::Release);
    Some(KeyEvent {
        scancode: (encoded & 0x7F) as u8,
        pressed: encoded & (1 << 7) != 0,
        extended: encoded & (1 << 8) != 0,
    })
}

pub fn dropped_events() -> u32 { DROPPED.load(Ordering::Relaxed) }

/// Translate common non-extended set-1 make codes to ASCII. Modifier state is
/// intentionally left to the desktop/input policy layer.
pub fn scancode_to_ascii(code: u8) -> Option<u8> {
    Some(match code {
        0x02..=0x0A => b'1' + (code - 0x02),
        0x0B => b'0',
        0x0C => b'-', 0x0D => b'=', 0x0E => 8, 0x0F => b'\t',
        0x10 => b'q', 0x11 => b'w', 0x12 => b'e', 0x13 => b'r',
        0x14 => b't', 0x15 => b'y', 0x16 => b'u', 0x17 => b'i',
        0x18 => b'o', 0x19 => b'p', 0x1A => b'[', 0x1B => b']',
        0x1C => b'\n', 0x1E => b'a', 0x1F => b's', 0x20 => b'd',
        0x21 => b'f', 0x22 => b'g', 0x23 => b'h', 0x24 => b'j',
        0x25 => b'k', 0x26 => b'l', 0x27 => b';', 0x28 => b'\'',
        0x29 => 0x60, 0x2B => b'\\', 0x2C => b'z', 0x2D => b'x',
        0x2E => b'c', 0x2F => b'v', 0x30 => b'b', 0x31 => b'n',
        0x32 => b'm', 0x33 => b',', 0x34 => b'.', 0x35 => b'/',
        0x37 => b'*', 0x39 => b' ', _ => return None,
    })
}
