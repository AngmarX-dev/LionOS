//! 32-bit x86 Task State Segment used for privilege-level stack selection.
use core::arch::asm;
use core::mem::size_of;
use core::ptr::{addr_of_mut, write_volatile};

/// Hardware-defined 32-bit TSS layout (104 bytes).
#[repr(C, packed)]
struct TaskStateSegment {
    previous_task: u16, _reserved0: u16,
    esp0: u32, ss0: u16, _reserved1: u16,
    esp1: u32, ss1: u16, _reserved2: u16,
    esp2: u32, ss2: u16, _reserved3: u16,
    cr3: u32, eip: u32, eflags: u32,
    eax: u32, ecx: u32, edx: u32, ebx: u32,
    esp: u32, ebp: u32, esi: u32, edi: u32,
    es: u16, _reserved4: u16, cs: u16, _reserved5: u16,
    ss: u16, _reserved6: u16, ds: u16, _reserved7: u16,
    fs: u16, _reserved8: u16, gs: u16, _reserved9: u16,
    ldt_selector: u16, _reserved10: u16,
    trap: u16, io_map_base: u16,
}
impl TaskStateSegment {
    const fn new() -> Self {
        Self {
            previous_task: 0, _reserved0: 0, esp0: 0, ss0: 0x10, _reserved1: 0,
            esp1: 0, ss1: 0, _reserved2: 0, esp2: 0, ss2: 0, _reserved3: 0,
            cr3: 0, eip: 0, eflags: 0, eax: 0, ecx: 0, edx: 0, ebx: 0,
            esp: 0, ebp: 0, esi: 0, edi: 0,
            es: 0, _reserved4: 0, cs: 0, _reserved5: 0, ss: 0, _reserved6: 0,
            ds: 0, _reserved7: 0, fs: 0, _reserved8: 0, gs: 0, _reserved9: 0,
            ldt_selector: 0, _reserved10: 0, trap: 0,
            io_map_base: size_of::<Self>() as u16,
        }
    }
}
#[repr(align(16))]
struct TssStorage(TaskStateSegment);
static mut TSS: TssStorage = TssStorage(TaskStateSegment::new());
pub const TSS_SELECTOR: u16 = 0x18;

/// Install the TSS descriptor and load TR. Initial ESP0 is the current kernel stack.
pub unsafe fn init(initial_kernel_stack: u32) {
    let tss_ptr = addr_of_mut!(TSS.0);
    (*tss_ptr).esp0 = initial_kernel_stack;
    (*tss_ptr).ss0 = 0x10;
    (*tss_ptr).io_map_base = size_of::<TaskStateSegment>() as u16;
    crate::arch::gdt::install_tss_descriptor(tss_ptr as u32, (size_of::<TaskStateSegment>() - 1) as u32);
    asm!("ltr ax", in("ax") TSS_SELECTOR, options(nostack, preserves_flags));
}

/// Set the ring-0 stack top used on privilege transitions. Must be mapped and writable.
pub unsafe fn set_kernel_stack(stack_top: u32) -> bool {
    if stack_top == 0 || (stack_top & 0x3) != 0 { return false; }
    write_volatile(addr_of_mut!(TSS.0.esp0), stack_top);
    true
}
/// Read the configured ring-0 stack top.
pub fn kernel_stack() -> u32 {
    unsafe { core::ptr::read_volatile(core::ptr::addr_of!(TSS.0.esp0)) }
}
