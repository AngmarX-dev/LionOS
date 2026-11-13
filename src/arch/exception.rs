//! CPU exception diagnostics and conservative recovery policy.
//!
//! Only INT3 (#BP) is resumed automatically. Other CPU exceptions are
//! reported and halted because the kernel has no process isolation or
//! validated fault-recovery tables yet.
use core::arch::asm;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ExceptionClass {
    DivideError,
    Debug,
    NonMaskableInterrupt,
    Breakpoint,
    Overflow,
    BoundRange,
    InvalidOpcode,
    DeviceNotAvailable,
    DoubleFault,
    InvalidTss,
    SegmentNotPresent,
    StackSegmentFault,
    GeneralProtection,
    PageFault,
    FloatingPoint,
    AlignmentCheck,
    MachineCheck,
    SimdFloatingPoint,
    Virtualization,
    ControlProtection,
    VmmCommunication,
    Security,
    Reserved,
}

pub fn classify(vector: u32) -> ExceptionClass {
    match vector {
        0 => ExceptionClass::DivideError,
        1 => ExceptionClass::Debug,
        2 => ExceptionClass::NonMaskableInterrupt,
        3 => ExceptionClass::Breakpoint,
        4 => ExceptionClass::Overflow,
        5 => ExceptionClass::BoundRange,
        6 => ExceptionClass::InvalidOpcode,
        7 => ExceptionClass::DeviceNotAvailable,
        8 => ExceptionClass::DoubleFault,
        10 => ExceptionClass::InvalidTss,
        11 => ExceptionClass::SegmentNotPresent,
        12 => ExceptionClass::StackSegmentFault,
        13 => ExceptionClass::GeneralProtection,
        14 => ExceptionClass::PageFault,
        16 => ExceptionClass::FloatingPoint,
        17 => ExceptionClass::AlignmentCheck,
        18 => ExceptionClass::MachineCheck,
        19 => ExceptionClass::SimdFloatingPoint,
        20 => ExceptionClass::Virtualization,
        21 => ExceptionClass::ControlProtection,
        29 => ExceptionClass::VmmCommunication,
        30 => ExceptionClass::Security,
        _ => ExceptionClass::Reserved,
    }
}

fn serial_byte(byte: u8) {
    unsafe {
        for _ in 0..50_000 {
            if crate::arch::x86::inb(0x3FD) & 0x20 != 0 {
                crate::arch::x86::outb(0x3F8, byte);
                return;
            }
            crate::arch::x86::pause();
        }
    }
}
fn write_str(value: &str) {
    for byte in value.bytes() { serial_byte(byte); }
}
fn write_hex(value: u32) {
    const DIGITS: &[u8; 16] = b"0123456789ABCDEF";
    write_str("0x");
    for shift in (0..8).rev() {
        serial_byte(DIGITS[((value >> (shift * 4)) & 0xF) as usize]);
    }
}
fn write_class(class: ExceptionClass) {
    write_str(match class {
        ExceptionClass::DivideError => "#DE Divide Error",
        ExceptionClass::Debug => "#DB Debug",
        ExceptionClass::NonMaskableInterrupt => "NMI",
        ExceptionClass::Breakpoint => "#BP Breakpoint",
        ExceptionClass::Overflow => "#OF Overflow",
        ExceptionClass::BoundRange => "#BR Bound Range",
        ExceptionClass::InvalidOpcode => "#UD Invalid Opcode",
        ExceptionClass::DeviceNotAvailable => "#NM Device Not Available",
        ExceptionClass::DoubleFault => "#DF Double Fault",
        ExceptionClass::InvalidTss => "#TS Invalid TSS",
        ExceptionClass::SegmentNotPresent => "#NP Segment Not Present",
        ExceptionClass::StackSegmentFault => "#SS Stack Segment Fault",
        ExceptionClass::GeneralProtection => "#GP General Protection",
        ExceptionClass::PageFault => "#PF Page Fault",
        ExceptionClass::FloatingPoint => "#MF x87 Floating Point",
        ExceptionClass::AlignmentCheck => "#AC Alignment Check",
        ExceptionClass::MachineCheck => "#MC Machine Check",
        ExceptionClass::SimdFloatingPoint => "#XM SIMD Floating Point",
        ExceptionClass::Virtualization => "#VE Virtualization",
        ExceptionClass::ControlProtection => "#CP Control Protection",
        ExceptionClass::VmmCommunication => "#VC VMM Communication",
        ExceptionClass::Security => "#SX Security",
        ExceptionClass::Reserved => "Reserved/Unknown Exception",
    });
}

/// Report an exception. Returns true only when the interrupted instruction
/// stream is known to be safely resumable (INT3 trap).
pub fn handle(vector: u32, error_code: u32, eip: u32, cs: u32, eflags: u32) -> bool {
    unsafe { crate::arch::x86::interrupts_disable(); }
    let class = classify(vector);
    write_str("\r\n[LIONOS EXCEPTION] ");
    write_class(class);
    write_str("\r\n vector="); write_hex(vector);
    write_str(" error="); write_hex(error_code);
    write_str("\r\n eip="); write_hex(eip);
    write_str(" cs="); write_hex(cs);
    write_str(" eflags="); write_hex(eflags);
    if vector == 14 {
        let cr2: u32;
        unsafe { asm!("mov {0}, cr2", out(reg) cr2, options(nomem, nostack, preserves_flags)); }
        write_str("\r\n cr2="); write_hex(cr2);
        write_str(" pf[");
        if error_code & 1 != 0 { write_str(" protection") } else { write_str(" not-present") }
        if error_code & 2 != 0 { write_str(" write") } else { write_str(" read") }
        if error_code & 4 != 0 { write_str(" user") } else { write_str(" supervisor") }
        if error_code & 8 != 0 { write_str(" reserved-bit") }
        if error_code & 16 != 0 { write_str(" instruction-fetch") }
        if error_code & 32 != 0 { write_str(" protection-key") }
        if error_code & 64 != 0 { write_str(" shadow-stack") }
        if error_code & 0x8000 != 0 { write_str(" sgx") }
        write_str(" ]");
    }
    write_str("\r\n");
    if vector == 3 {
        write_str("[RECOVERY] INT3 breakpoint: resuming after trap.\r\n");
        return true;
    }
    write_str("[FATAL] Kernel state is not recoverable; CPU halted.\r\n");
    loop { unsafe { asm!("cli; hlt", options(nomem, nostack)); } }
}
