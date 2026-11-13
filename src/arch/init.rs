//! Ordered early x86 architecture bring-up.
//! Runs with maskable interrupts disabled and deliberately leaves IF clear.
#[derive(Clone, Copy, Debug)]
pub struct EarlyInitReport {
    pub gdt_tss_loaded: bool,
    pub idt_loaded: bool,
    pub legacy_pic_remapped: bool,
    pub pit_programmed: bool,
}

pub unsafe fn early_init() -> EarlyInitReport {
    crate::arch::x86::interrupts_disable();
    crate::arch::gdt::init();
    crate::arch::interrupts::init();
    crate::arch::interrupts::timer_init();
    EarlyInitReport {
        gdt_tss_loaded: true,
        idt_loaded: true,
        legacy_pic_remapped: true,
        pit_programmed: true,
    }
}
