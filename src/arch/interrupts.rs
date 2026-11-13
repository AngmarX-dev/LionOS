//! 32-bit x86 IDT, exception entry, legacy PIC and PIT timer.
use core::arch::{asm, global_asm};
use core::ptr::{addr_of, read_volatile};
use core::mem::size_of;

global_asm!(r#".intel_syntax noprefix
.code32
.section .text
.extern lionos_interrupt_dispatch
.global lionos_isr0
lionos_isr0:
  push 0
  push 0
  jmp lionos_isr_common
.global lionos_isr1
lionos_isr1:
  push 0
  push 1
  jmp lionos_isr_common
.global lionos_isr2
lionos_isr2:
  push 0
  push 2
  jmp lionos_isr_common
.global lionos_isr3
lionos_isr3:
  push 0
  push 3
  jmp lionos_isr_common
.global lionos_isr4
lionos_isr4:
  push 0
  push 4
  jmp lionos_isr_common
.global lionos_isr5
lionos_isr5:
  push 0
  push 5
  jmp lionos_isr_common
.global lionos_isr6
lionos_isr6:
  push 0
  push 6
  jmp lionos_isr_common
.global lionos_isr7
lionos_isr7:
  push 0
  push 7
  jmp lionos_isr_common
.global lionos_isr8
lionos_isr8:
  push 8
  jmp lionos_isr_common
.global lionos_isr9
lionos_isr9:
  push 0
  push 9
  jmp lionos_isr_common
.global lionos_isr10
lionos_isr10:
  push 10
  jmp lionos_isr_common
.global lionos_isr11
lionos_isr11:
  push 11
  jmp lionos_isr_common
.global lionos_isr12
lionos_isr12:
  push 12
  jmp lionos_isr_common
.global lionos_isr13
lionos_isr13:
  push 13
  jmp lionos_isr_common
.global lionos_isr14
lionos_isr14:
  push 14
  jmp lionos_isr_common
.global lionos_isr15
lionos_isr15:
  push 0
  push 15
  jmp lionos_isr_common
.global lionos_isr16
lionos_isr16:
  push 0
  push 16
  jmp lionos_isr_common
.global lionos_isr17
lionos_isr17:
  push 17
  jmp lionos_isr_common
.global lionos_isr18
lionos_isr18:
  push 0
  push 18
  jmp lionos_isr_common
.global lionos_isr19
lionos_isr19:
  push 0
  push 19
  jmp lionos_isr_common
.global lionos_isr20
lionos_isr20:
  push 20
  jmp lionos_isr_common
.global lionos_isr21
lionos_isr21:
  push 21
  jmp lionos_isr_common
.global lionos_isr22
lionos_isr22:
  push 0
  push 22
  jmp lionos_isr_common
.global lionos_isr23
lionos_isr23:
  push 0
  push 23
  jmp lionos_isr_common
.global lionos_isr24
lionos_isr24:
  push 0
  push 24
  jmp lionos_isr_common
.global lionos_isr25
lionos_isr25:
  push 0
  push 25
  jmp lionos_isr_common
.global lionos_isr26
lionos_isr26:
  push 0
  push 26
  jmp lionos_isr_common
.global lionos_isr27
lionos_isr27:
  push 0
  push 27
  jmp lionos_isr_common
.global lionos_isr28
lionos_isr28:
  push 0
  push 28
  jmp lionos_isr_common
.global lionos_isr29
lionos_isr29:
  push 29
  jmp lionos_isr_common
.global lionos_isr30
lionos_isr30:
  push 30
  jmp lionos_isr_common
.global lionos_isr31
lionos_isr31:
  push 0
  push 31
  jmp lionos_isr_common
.global lionos_isr32
lionos_isr32:
  push 0
  push 32
  jmp lionos_isr_common
.global lionos_isr33
lionos_isr33:
  push 0
  push 33
  jmp lionos_isr_common
.global lionos_isr34
lionos_isr34:
  push 0
  push 34
  jmp lionos_isr_common
.global lionos_isr35
lionos_isr35:
  push 0
  push 35
  jmp lionos_isr_common
.global lionos_isr36
lionos_isr36:
  push 0
  push 36
  jmp lionos_isr_common
.global lionos_isr37
lionos_isr37:
  push 0
  push 37
  jmp lionos_isr_common
.global lionos_isr38
lionos_isr38:
  push 0
  push 38
  jmp lionos_isr_common
.global lionos_isr39
lionos_isr39:
  push 0
  push 39
  jmp lionos_isr_common
.global lionos_isr40
lionos_isr40:
  push 0
  push 40
  jmp lionos_isr_common
.global lionos_isr41
lionos_isr41:
  push 0
  push 41
  jmp lionos_isr_common
.global lionos_isr42
lionos_isr42:
  push 0
  push 42
  jmp lionos_isr_common
.global lionos_isr43
lionos_isr43:
  push 0
  push 43
  jmp lionos_isr_common
.global lionos_isr44
lionos_isr44:
  push 0
  push 44
  jmp lionos_isr_common
.global lionos_isr45
lionos_isr45:
  push 0
  push 45
  jmp lionos_isr_common
.global lionos_isr46
lionos_isr46:
  push 0
  push 46
  jmp lionos_isr_common
.global lionos_isr47
lionos_isr47:
  push 0
  push 47
  jmp lionos_isr_common
.global lionos_default_isr
lionos_default_isr:
  push 0
  push 255
  jmp lionos_isr_common
lionos_isr_common:
  cld
  pushad
  push ds
  push es
  push fs
  push gs
  mov ax, 0x10
  mov ds, ax
  mov es, ax
  mov fs, ax
  mov gs, ax
  mov ebx, esp
  and esp, 0xfffffff0
  sub esp, 12
  push ebx
  call lionos_interrupt_dispatch
  mov esp, ebx
  pop gs
  pop fs
  pop es
  pop ds
  popad
  add esp, 8
  iretd
.section .rodata
.align 4
.global lionos_isr_stub_table
lionos_isr_stub_table:
  .long lionos_isr0
  .long lionos_isr1
  .long lionos_isr2
  .long lionos_isr3
  .long lionos_isr4
  .long lionos_isr5
  .long lionos_isr6
  .long lionos_isr7
  .long lionos_isr8
  .long lionos_isr9
  .long lionos_isr10
  .long lionos_isr11
  .long lionos_isr12
  .long lionos_isr13
  .long lionos_isr14
  .long lionos_isr15
  .long lionos_isr16
  .long lionos_isr17
  .long lionos_isr18
  .long lionos_isr19
  .long lionos_isr20
  .long lionos_isr21
  .long lionos_isr22
  .long lionos_isr23
  .long lionos_isr24
  .long lionos_isr25
  .long lionos_isr26
  .long lionos_isr27
  .long lionos_isr28
  .long lionos_isr29
  .long lionos_isr30
  .long lionos_isr31
  .long lionos_isr32
  .long lionos_isr33
  .long lionos_isr34
  .long lionos_isr35
  .long lionos_isr36
  .long lionos_isr37
  .long lionos_isr38
  .long lionos_isr39
  .long lionos_isr40
  .long lionos_isr41
  .long lionos_isr42
  .long lionos_isr43
  .long lionos_isr44
  .long lionos_isr45
  .long lionos_isr46
  .long lionos_isr47
.section .note.GNU-stack,"",@progbits
"#);

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct IdtEntry { offset_low:u16, selector:u16, zero:u8, attributes:u8, offset_high:u16 }
impl IdtEntry {
    const EMPTY:Self=Self{offset_low:0,selector:0,zero:0,attributes:0,offset_high:0};
    fn new(handler:u32)->Self { Self{offset_low:handler as u16,selector:0x08,zero:0,attributes:0x8E,offset_high:(handler>>16) as u16} }
}
#[repr(C, packed)]
struct Idtr { limit:u16, base:u32 }
#[repr(C)]
struct InterruptFrame {
    gs:u32, fs:u32, es:u32, ds:u32,
    edi:u32, esi:u32, ebp:u32, saved_esp:u32,
    ebx:u32, edx:u32, ecx:u32, eax:u32,
    vector:u32, error_code:u32,
    eip:u32, cs:u32, eflags:u32,
}
static mut IDT:[IdtEntry;256]=[IdtEntry::EMPTY;256];
static mut TIMER_TICKS:u64=0;
extern "C" { static lionos_isr_stub_table:[u32;48]; fn lionos_default_isr(); }

#[inline]
unsafe fn pic_remap_and_mask() {
    use crate::arch::x86::{inb,io_wait,outb};
    let _saved_masks=(inb(0x21),inb(0xA1));
    outb(0x20,0x11); io_wait(); outb(0xA0,0x11); io_wait();
    outb(0x21,0x20); io_wait(); outb(0xA1,0x28); io_wait();
    outb(0x21,0x04); io_wait(); outb(0xA1,0x02); io_wait();
    outb(0x21,0x01); io_wait(); outb(0xA1,0x01); io_wait();
    outb(0x21,0xFF); outb(0xA1,0xFF);
}

pub unsafe fn init() {
    let fallback=IdtEntry::new(lionos_default_isr as usize as u32);
    for vector in 0..256 { IDT[vector]=fallback; }
    for vector in 0..48 {
        let handler=read_volatile(addr_of!(lionos_isr_stub_table).cast::<u32>().add(vector));
        IDT[vector]=IdtEntry::new(handler);
    }
    let idtr=Idtr{limit:(size_of::<[IdtEntry;256]>()-1) as u16,base:addr_of!(IDT) as u32};
    asm!("lidt [{0}]",in(reg)&idtr,options(readonly,nostack,preserves_flags));
    pic_remap_and_mask();
}

/// Program PIT channel 0 at nominal 100 Hz; leave all but IRQ0 masked.
pub unsafe fn timer_init() {
    use crate::arch::x86::outb;
    const PIT_INPUT_HZ:u32=1_193_182;
    let divisor=(PIT_INPUT_HZ/100) as u16;
    outb(0x43,0x36);
    outb(0x40,(divisor&0xFF) as u8);
    outb(0x40,(divisor>>8) as u8);
    outb(0x21,0xFE);
    outb(0xA1,0xFF);
}

/// Unmask the cascaded PIC path for PS/2 auxiliary IRQ12.
pub unsafe fn enable_mouse_irq() {
    use crate::arch::x86::{inb, outb};
    let master = inb(0x21) & !(1 << 2); // cascade IRQ2
    let slave = inb(0xA1) & !(1 << 4);  // IRQ12 on the slave PIC
    outb(0x21, master);
    outb(0xA1, slave);
}

/// Monotonic PIT tick counter (nominally 100 ticks per second).
/// The snapshot is taken with maskable interrupts disabled so the 64-bit read
/// cannot tear on this 32-bit target; the caller's IF state is then restored.
pub fn ticks()->u64 {
    let flags:u32;
    unsafe {
        asm!("pushfd; pop {0}; cli",out(reg) flags,options(nomem));
        let snapshot=read_volatile(addr_of!(TIMER_TICKS));
        if flags & (1<<9)!=0 { asm!("sti",options(nomem,nostack)); }
        snapshot
    }
}

#[no_mangle]
pub extern "C" fn lionos_interrupt_dispatch(frame:*const InterruptFrame) {
    let vector=unsafe { read_volatile(addr_of!((*frame).vector)) };
    if vector==32 {
        unsafe { TIMER_TICKS=TIMER_TICKS.wrapping_add(1); crate::scheduler::on_timer_tick(); crate::arch::x86::outb(0x20,0x20); }
        return;
    }
    if vector == 44 {
        // IRQ12: consume exactly one auxiliary byte before acknowledging both PICs.
        unsafe {
            let byte = crate::arch::x86::inb(0x60);
            crate::mouse::handle_irq_byte(byte);
            crate::arch::x86::outb(0xA0, 0x20);
            crate::arch::x86::outb(0x20, 0x20);
        }
        return;
    }
    if (32..48).contains(&vector) {
        unsafe {
            if vector>=40 { crate::arch::x86::outb(0xA0,0x20); }
            crate::arch::x86::outb(0x20,0x20);
        }
        return;
    }
    if vector < 32 {
        let error_code=unsafe { read_volatile(addr_of!((*frame).error_code)) };
        let eip=unsafe { read_volatile(addr_of!((*frame).eip)) };
        let cs=unsafe { read_volatile(addr_of!((*frame).cs)) };
        let eflags=unsafe { read_volatile(addr_of!((*frame).eflags)) };
        let _=crate::arch::exception::handle(vector,error_code,eip,cs,eflags);
        return;
    }
    // Unexpected vectors outside the installed exception/IRQ ranges fail closed.
    unsafe { crate::arch::x86::interrupts_disable(); }
    loop { unsafe { asm!("hlt",options(nomem,nostack)); } }
}
