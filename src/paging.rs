//! Initial 32-bit PSE identity map. This is a bring-up map, not process isolation.
use core::arch::asm;
use core::ptr::{read_volatile,write_volatile};

#[repr(align(4096))]
struct PageDirectory([u32;1024]);
static mut DIRECTORY:PageDirectory=PageDirectory([0;1024]);
static mut PAGING_READY:bool=false;
const LARGE_PAGE_SIZE:u64=4*1024*1024;
const PDE_PWT:u32=1<<3;
const PDE_PCD:u32=1<<4;

/// Enable a supervisor-only, writable 4 GiB identity map using 4 MiB pages.
pub unsafe fn init_identity_4g()->bool{
    if !crate::arch::cpu::features().pse{return false;}
    let directory=core::ptr::addr_of_mut!(DIRECTORY.0).cast::<u32>();
    for index in 0..1024usize{
        let entry=((index as u32)<<22)|0x083;
        write_volatile(directory.add(index),entry);
    }
    let physical=directory as u32;
    asm!(
        "mov eax, cr4","or eax, 0x10","mov cr4, eax",
        "mov cr3, ecx","mov eax, cr0","or eax, 0x80000000",
        "mov cr0, eax","jmp 2f","2:",
        in("ecx") physical,out("eax") _,options(nostack),
    );
    PAGING_READY=true;
    true
}

/// Report whether the temporary identity map is active.
pub fn is_ready()->bool{unsafe{PAGING_READY}}

/// Mark the 4 MiB identity-mapped PDEs covering a physical MMIO range as
/// uncached (PWT+PCD), then flush the TLB. This is coarse-grained because the
/// current bootstrap map uses 4 MiB pages; it is not a general MMIO mapper.
pub unsafe fn mark_mmio_uncacheable(base:u64,length:usize)->bool{
    if !PAGING_READY||length==0{return false;}
    let end=match base.checked_add(length as u64){Some(v)=>v,None=>return false};
    if end>0x1_0000_0000||base>=end{return false;}
    let first=(base/LARGE_PAGE_SIZE) as usize;
    let last=((end-1)/LARGE_PAGE_SIZE) as usize;
    let directory=core::ptr::addr_of_mut!(DIRECTORY.0).cast::<u32>();
    for index in first..=last{
        let entry=read_volatile(directory.add(index));
        if entry&1==0||entry&(1<<7)==0{return false;}
        write_volatile(directory.add(index),entry|PDE_PWT|PDE_PCD);
    }
    let physical=directory as u32;
    asm!("mov cr3, {}",in(reg) physical,options(nostack,preserves_flags));
    true
}
