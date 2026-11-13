//! Minimal protected-mode IDT and legacy PIC masking.
//! All vectors currently enter a fail-stop handler; individual exception/IRQ
//! handlers and APIC support are follow-up work.
use core::arch::{asm, global_asm};
use core::mem::size_of;

global_asm!(
    ".global lionos_default_isr",
    "lionos_default_isr:",
    "cli",
    "pusha",
    "call lionos_unhandled_interrupt",
    "1: hlt",
    "jmp 1b",
);

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct IdtEntry { offset_low:u16, selector:u16, zero:u8, attributes:u8, offset_high:u16 }
impl IdtEntry { const EMPTY:Self=Self{offset_low:0,selector:0,zero:0,attributes:0,offset_high:0}; }
#[repr(C, packed)]
struct Idtr { limit:u16, base:u32 }
static mut IDT:[IdtEntry;256]=[IdtEntry::EMPTY;256];
extern "C" { fn lionos_default_isr(); }

#[no_mangle]
pub extern "C" fn lionos_unhandled_interrupt() -> ! {
    loop { unsafe { asm!("cli; hlt", options(nomem,nostack)); } }
}

pub unsafe fn init() {
    let handler=lionos_default_isr as usize as u32;
    let entry=IdtEntry{
        offset_low:handler as u16, selector:0x08, zero:0,
        attributes:0x8E, offset_high:(handler>>16) as u16,
    };
    for vector in 0..256 { IDT[vector]=entry; }
    let idtr=Idtr{limit:(size_of::<[IdtEntry;256]>()-1) as u16,base:core::ptr::addr_of!(IDT) as u32};
    asm!("lidt [{0}]",in(reg)&idtr,options(readonly,nostack,preserves_flags));
    // Remap the legacy PIC to 0x20/0x28, then keep every IRQ masked until handlers exist.
    use crate::arch::x86::{inb,outb};
    let master=inb(0x21); let slave=inb(0xA1);
    outb(0x20,0x11); outb(0xA0,0x11);
    outb(0x21,0x20); outb(0xA1,0x28);
    outb(0x21,0x04); outb(0xA1,0x02);
    outb(0x21,0x01); outb(0xA1,0x01);
    let _ = (master,slave);
    outb(0x21,0xFF); outb(0xA1,0xFF);
}
