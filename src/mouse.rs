//! Bounded PS/2 auxiliary-port mouse driver for the legacy 8042 controller.
//! Initialization uses finite polling; packet decoding runs from IRQ12.
use crate::arch::x86::{inb, outb, io_wait, interrupts_disable, interrupts_enable};

const STATUS: u16 = 0x64;
const DATA: u16 = 0x60;
const WAIT_LIMIT: usize = 100_000;
const ACK: u8 = 0xFA;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MouseInitError {
    ControllerTimeout,
    MouseTimeout,
    CommandRejected,
    UnsupportedDevice,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct MouseSnapshot {
    pub x: i32,
    pub y: i32,
    pub buttons: u8,
    pub wheel: i16,
    pub packets: u32,
    pub initialized: bool,
}

struct Decoder {
    packet: [u8; 4],
    used: u8,
    packet_len: u8,
    x: i32,
    y: i32,
    max_x: i32,
    max_y: i32,
    buttons: u8,
    wheel: i16,
    packets: u32,
    initialized: bool,
}

impl Decoder {
    const fn new() -> Self {
        Self { packet: [0; 4], used: 0, packet_len: 3, x: 0, y: 0,
            max_x: 639, max_y: 479, buttons: 0, wheel: 0, packets: 0,
            initialized: false }
    }

    fn push(&mut self, byte: u8) {
        if self.used == 0 && byte & 0x08 == 0 { return; }
        self.packet[self.used as usize] = byte;
        self.used += 1;
        if self.used < self.packet_len { return; }
        self.used = 0;

        let flags = self.packet[0];
        if flags & 0xC0 != 0 { return; } // discard X/Y overflow packets
        let dx = if flags & 0x10 != 0 {
            self.packet[1] as i32 - 256
        } else { self.packet[1] as i32 };
        let dy = if flags & 0x20 != 0 {
            self.packet[2] as i32 - 256
        } else { self.packet[2] as i32 };

        self.x = self.x.saturating_add(dx).clamp(0, self.max_x);
        self.y = self.y.saturating_sub(dy).clamp(0, self.max_y);
        self.buttons = flags & 0x07;
        if self.packet_len == 4 {
            let nibble = (self.packet[3] & 0x0F) as i8;
            let wheel = if nibble & 0x08 != 0 { nibble - 16 } else { nibble };
            self.wheel = self.wheel.saturating_add(wheel as i16);
            if self.packet[3] & 0x10 != 0 { self.buttons |= 1 << 3; }
            if self.packet[3] & 0x20 != 0 { self.buttons |= 1 << 4; }
        }
        self.packets = self.packets.wrapping_add(1);
    }
}

static mut DECODER: Decoder = Decoder::new();

unsafe fn wait_input_empty() -> Result<(), MouseInitError> {
    for _ in 0..WAIT_LIMIT {
        if inb(STATUS) & 0x02 == 0 { return Ok(()); }
        core::hint::spin_loop();
    }
    Err(MouseInitError::ControllerTimeout)
}

unsafe fn wait_output_any() -> Result<u8, MouseInitError> {
    for _ in 0..WAIT_LIMIT {
        if inb(STATUS) & 0x01 != 0 { return Ok(inb(DATA)); }
        core::hint::spin_loop();
    }
    Err(MouseInitError::ControllerTimeout)
}

unsafe fn wait_aux_output() -> Result<u8, MouseInitError> {
    for _ in 0..WAIT_LIMIT {
        let status = inb(STATUS);
        if status & 0x01 != 0 {
            let value = inb(DATA);
            if status & 0x20 != 0 { return Ok(value); }
            // Drain pending keyboard data while waiting for an auxiliary reply.
        }
        core::hint::spin_loop();
    }
    Err(MouseInitError::MouseTimeout)
}

unsafe fn controller_command(command: u8) -> Result<(), MouseInitError> {
    wait_input_empty()?;
    outb(STATUS, command);
    io_wait();
    Ok(())
}

unsafe fn write_controller_data(value: u8) -> Result<(), MouseInitError> {
    wait_input_empty()?;
    outb(DATA, value);
    io_wait();
    Ok(())
}

unsafe fn send_aux(value: u8) -> Result<(), MouseInitError> {
    controller_command(0xD4)?;
    write_controller_data(value)?;
    match wait_aux_output()? {
        ACK => Ok(()),
        _ => Err(MouseInitError::CommandRejected),
    }
}

unsafe fn set_sample_rate(rate: u8) -> Result<(), MouseInitError> {
    send_aux(0xF3)?;
    send_aux(rate)
}

/// Initialize the auxiliary PS/2 port and enable IRQ12 after the mouse ACKs.
/// Call with maskable interrupts disabled; every controller wait is bounded.
pub unsafe fn initialize() -> Result<(), MouseInitError> {
    interrupts_disable();
    controller_command(0xA7)?; // disable auxiliary port during setup
    for _ in 0..WAIT_LIMIT {
        if inb(STATUS) & 0x01 == 0 { break; }
        let _ = inb(DATA);
    }

    controller_command(0x20)?;
    let mut config = wait_output_any()?;

    config |= 0x02;  // enable IRQ12
    config &= !0x20; // enable auxiliary clock
    controller_command(0x60)?;
    write_controller_data(config)?;
    controller_command(0xA8)?; // enable auxiliary port

    send_aux(0xF6)?; // defaults
    set_sample_rate(200)?;
    set_sample_rate(100)?;
    set_sample_rate(80)?;
    send_aux(0xF2)?; // identify device
    let mut device_id = wait_aux_output()?;
    if device_id == 0x03 {
        // Explorer extension: request the five-button wheel report format.
        set_sample_rate(200)?;
        set_sample_rate(200)?;
        set_sample_rate(80)?;
        send_aux(0xF2)?;
        device_id = wait_aux_output()?;
    }
    let packet_len = match device_id {
        0x00 => 3,
        0x03 | 0x04 => 4,
        _ => return Err(MouseInitError::UnsupportedDevice),
    };
    send_aux(0xF4)?; // enable data reporting

    let state = core::ptr::addr_of_mut!(DECODER);
    (*state).used = 0;
    (*state).packet_len = packet_len;
    (*state).buttons = 0;
    (*state).wheel = 0;
    (*state).packets = 0;
    (*state).initialized = true;
    crate::arch::interrupts::enable_mouse_irq();
    Ok(())
}

/// Called only by the IRQ12 interrupt path after reading the auxiliary data byte.
pub fn handle_irq_byte(byte: u8) {
    unsafe { (*core::ptr::addr_of_mut!(DECODER)).push(byte); }
}

/// Set screen bounds and place the pointer at its center.
pub fn set_screen_bounds(width: u32, height: u32) {
    let flags: u32;
    unsafe {
        core::arch::asm!("pushfd; pop {0}; cli", out(reg) flags, options(nomem));
        let state = core::ptr::addr_of_mut!(DECODER);
        (*state).max_x = width.saturating_sub(12).min(i32::MAX as u32) as i32;
        (*state).max_y = height.saturating_sub(16).min(i32::MAX as u32) as i32;
        (*state).x = (*state).max_x / 2;
        (*state).y = (*state).max_y / 2;
        if flags & (1 << 9) != 0 { interrupts_enable(); }
    }
}

/// Snapshot shared IRQ state atomically on the single-core bootstrap.
pub fn snapshot() -> MouseSnapshot {
    let flags: u32;
    unsafe {
        core::arch::asm!("pushfd; pop {0}; cli", out(reg) flags, options(nomem));
        let state = core::ptr::addr_of!(DECODER);
        let snapshot = MouseSnapshot {
            x: (*state).x, y: (*state).y, buttons: (*state).buttons, wheel: (*state).wheel,
            packets: (*state).packets, initialized: (*state).initialized,
        };
        if flags & (1 << 9) != 0 { interrupts_enable(); }
        snapshot
    }
}
