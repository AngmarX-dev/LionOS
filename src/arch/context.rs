//! Stable software context layouts for 32-bit x86 task management.
//! These declarations define an ABI only; no context switch is active yet.

/// State contract for a future cooperative kernel context-switch routine.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct KernelContext {
    pub esp: u32, pub eip: u32, pub ebp: u32, pub ebx: u32,
    pub esi: u32, pub edi: u32, pub eflags: u32, pub cr3: u32,
}
impl KernelContext {
    pub const fn empty() -> Self {
        Self { esp: 0, eip: 0, ebp: 0, ebx: 0, esi: 0, edi: 0, eflags: 0x2, cr3: 0 }
    }
    pub fn is_valid(&self) -> bool {
        self.esp != 0 && self.esp & 3 == 0 && self.eip != 0
            && self.eflags & 2 != 0 && (self.cr3 == 0 || self.cr3 & 0xFFF == 0)
    }
}

/// Register frame laid out by the common ISR entry stub in arch/interrupts.rs.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct InterruptContext {
    pub gs: u32, pub fs: u32, pub es: u32, pub ds: u32,
    pub edi: u32, pub esi: u32, pub ebp: u32, pub saved_esp: u32,
    pub ebx: u32, pub edx: u32, pub ecx: u32, pub eax: u32,
    pub vector: u32, pub error_code: u32, pub eip: u32, pub cs: u32, pub eflags: u32,
}
impl InterruptContext {
    pub fn came_from_user(&self) -> bool { self.cs & 3 == 3 }
}
