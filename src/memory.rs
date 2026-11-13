//! Fixed-capacity physical page-frame allocator (4 KiB frames, 32-bit physical space).
use core::ptr::read_unaligned;
use core::sync::atomic::{AtomicBool,Ordering};
use core::arch::asm;

pub const PAGE_SIZE:u32=4096;
const FRAME_COUNT:usize=1_048_576;
const WORD_COUNT:usize=FRAME_COUNT/32;
const PHYSICAL_LIMIT:u64=0x1_0000_0000;
static mut USED:[u32;WORD_COUNT]=[u32::MAX;WORD_COUNT];
static mut ALLOCATED:[u32;WORD_COUNT]=[0;WORD_COUNT];
static mut READY:bool=false;
static mut FREE_FRAMES:u32=0;
static ALLOCATOR_LOCK:AtomicBool=AtomicBool::new(false);

#[derive(Clone,Copy,Debug)]
pub struct MemoryStats{pub total_usable_bytes:u64,pub free_frames:u32,pub initialized:bool}

/// Serialize allocator metadata. Interrupts are disabled while held so a same-CPU
/// interrupt cannot re-enter the allocator and deadlock on this lock.
struct AllocatorGuard{restore_interrupts:bool}
impl AllocatorGuard{
    fn acquire()->Self{
        let flags:u32;
        unsafe{asm!("pushfd; pop {0}; cli",out(reg) flags);}
        while ALLOCATOR_LOCK.compare_exchange_weak(false,true,Ordering::Acquire,Ordering::Relaxed).is_err(){
            core::hint::spin_loop();
        }
        Self{restore_interrupts:flags&(1<<9)!=0}
    }
}
impl Drop for AllocatorGuard{
    fn drop(&mut self){
        ALLOCATOR_LOCK.store(false,Ordering::Release);
        if self.restore_interrupts{unsafe{asm!("sti",options(nomem,nostack));}}
    }
}

unsafe fn set_bit(bitmap:*mut u32,frame:usize,value:bool){
    let word=bitmap.add(frame/32);
    let mask=1u32<<(frame%32);
    let old=core::ptr::read_volatile(word);
    core::ptr::write_volatile(word,if value{old|mask}else{old&!mask});
}
unsafe fn bit(bitmap:*const u32,frame:usize)->bool{
    (core::ptr::read_volatile(bitmap.add(frame/32))&(1u32<<(frame%32)))!=0
}

/// Mark a physical interval. Free intervals are rounded inward; reserved
/// intervals are rounded outward so a partial page is never accidentally freed.
unsafe fn mark_region(base:u64,length:u64,reserved:bool){
    if length==0{return;}
    let end=base.saturating_add(length).min(PHYSICAL_LIMIT);
    let start_frame=(if reserved{base >> 12}else{base.saturating_add(4095) >> 12}) as usize;
    let end_frame=(if reserved{end.saturating_add(4095) >> 12}else{end >> 12}) as usize;
    for frame in start_frame.min(FRAME_COUNT)..end_frame.min(FRAME_COUNT){
        let was_used=bit(core::ptr::addr_of!(USED).cast::<u32>(),frame);
        if was_used&&!reserved{FREE_FRAMES=FREE_FRAMES.saturating_add(1);}
        if !was_used&&reserved{FREE_FRAMES=FREE_FRAMES.saturating_sub(1);}
        set_bit(core::ptr::addr_of_mut!(USED).cast::<u32>(),frame,reserved);
    }
}

/// Initialize from a validated, identity-mapped Multiboot2 information structure.
pub unsafe fn init(info:*const u8,total_size:usize)->bool{
    READY=false;
    if info.is_null()||total_size<16{return false;}
    for i in 0..WORD_COUNT{
        core::ptr::write_volatile(core::ptr::addr_of_mut!(USED).cast::<u32>().add(i),u32::MAX);
        core::ptr::write_volatile(core::ptr::addr_of_mut!(ALLOCATED).cast::<u32>().add(i),0);
    }
    FREE_FRAMES=0;
    let mut offset=8usize;
    let mut map_seen=false;
    while offset.checked_add(8).map_or(false,|v|v<=total_size){
        let tag=info.add(offset);
        let kind=read_unaligned(tag.cast::<u32>());
        let size=read_unaligned(tag.add(4).cast::<u32>()) as usize;
        if size<8||offset.checked_add(size).map_or(true,|v|v>total_size){return false;}
        if kind==0{break;}
        if kind==6{
            if size<16{return false;}
            let entry_size=read_unaligned(tag.add(8).cast::<u32>()) as usize;
            if entry_size<24{return false;}
            map_seen=true;
            let mut pos=16usize;
            while pos.checked_add(entry_size).map_or(false,|v|v<=size){
                let entry=tag.add(pos);
                let base=read_unaligned(entry.cast::<u64>());
                let length=read_unaligned(entry.add(8).cast::<u64>());
                let typ=read_unaligned(entry.add(16).cast::<u32>());
                if typ==1{mark_region(base,length,false);}
                pos+=entry_size;
            }
        }
        offset=(offset+size+7)&!7;
    }
    if !map_seen{return false;}

    mark_region(0,0x10_0000,true);
    extern "C"{static _kernel_start:u8;static _kernel_end:u8;}
    let kernel_start=core::ptr::addr_of!(_kernel_start) as u64;
    let kernel_end=core::ptr::addr_of!(_kernel_end) as u64;
    if kernel_end>kernel_start{mark_region(kernel_start,kernel_end-kernel_start,true);}
    mark_region(info as u32 as u64,total_size as u64,true);

    let mut pos=8usize;
    while pos+8<=total_size{
        let tag=info.add(pos);
        let kind=read_unaligned(tag.cast::<u32>());
        let size=read_unaligned(tag.add(4).cast::<u32>()) as usize;
        if size<8||pos.checked_add(size).map_or(true,|v|v>total_size){break;}
        if kind==0{break;}
        if kind==3&&size>=16{
            let start=read_unaligned(tag.add(8).cast::<u32>()) as u64;
            let end=read_unaligned(tag.add(12).cast::<u32>()) as u64;
            if end>start{mark_region(start,end-start,true);}
        }
        pos=(pos+size+7)&!7;
    }
    READY=true;
    true
}

/// Allocate physically contiguous pages with a power-of-two page alignment.
/// max_address is exclusive. A nonzero boundary prevents the allocation from
/// crossing that physical boundary (useful for controller DMA constraints).
pub fn alloc_pages_aligned(pages:u32,alignment_pages:u32,max_address:u64,boundary:u64)->Option<u32>{
    if pages==0||alignment_pages==0||!alignment_pages.is_power_of_two(){return None;}
    if max_address>PHYSICAL_LIMIT||max_address<(PAGE_SIZE as u64){return None;}
    if boundary!=0&&(!boundary.is_power_of_two()||boundary<(PAGE_SIZE as u64)){return None;}
    let limit=(max_address/(PAGE_SIZE as u64)) as usize;
    let needed=pages as usize;
    if needed>limit||needed>FRAME_COUNT{return None;}
    let _guard=AllocatorGuard::acquire();
    unsafe{
        if !READY{return None;}
        let mut start=0usize;
        while start.checked_add(needed).map_or(false,|end|end<=limit&&end<=FRAME_COUNT){
            let rem=start&(alignment_pages as usize-1);
            if rem!=0{start=start.checked_add(alignment_pages as usize-rem)?;continue;}
            let end=start+needed;
            let physical_start=(start as u64)*(PAGE_SIZE as u64);
            let physical_end=(end as u64)*(PAGE_SIZE as u64);
            if boundary!=0&&(physical_start/boundary)!=((physical_end-1)/boundary){
                let next_boundary=((physical_start/boundary)+1).checked_mul(boundary)?;
                start=(next_boundary/(PAGE_SIZE as u64)) as usize;
                continue;
            }
            let mut free=true;
            for frame in start..end{
                if bit(core::ptr::addr_of!(USED).cast::<u32>(),frame){free=false;start=frame+1;break;}
            }
            if free{
                for frame in start..end{
                    set_bit(core::ptr::addr_of_mut!(USED).cast::<u32>(),frame,true);
                    set_bit(core::ptr::addr_of_mut!(ALLOCATED).cast::<u32>(),frame,true);
                }
                FREE_FRAMES=FREE_FRAMES.saturating_sub(pages);
                return Some((start as u32)*PAGE_SIZE);
            }
        }
        None
    }
}
pub fn alloc_pages(pages:u32)->Option<u32>{
    alloc_pages_aligned(pages,1,PHYSICAL_LIMIT,0)
}
pub fn alloc_frame()->Option<u32>{alloc_pages(1)}

pub unsafe fn free_pages(address:u32,pages:u32)->bool{
    if !READY||pages==0||address%PAGE_SIZE!=0{return false;}
    let first=(address/PAGE_SIZE) as usize;
    let count=pages as usize;
    if first.checked_add(count).map_or(true,|end|end>FRAME_COUNT){return false;}
    let _guard=AllocatorGuard::acquire();
    if !READY{return false;}
    for frame in first..first+count{
        if !bit(core::ptr::addr_of!(ALLOCATED).cast::<u32>(),frame){return false;}
    }
    for frame in first..first+count{
        set_bit(core::ptr::addr_of_mut!(ALLOCATED).cast::<u32>(),frame,false);
        set_bit(core::ptr::addr_of_mut!(USED).cast::<u32>(),frame,false);
    }
    FREE_FRAMES=FREE_FRAMES.saturating_add(pages);
    true
}
pub unsafe fn free_frame(address:u32)->bool{free_pages(address,1)}

pub fn stats(total_usable_bytes:u64)->MemoryStats{
    let _guard=AllocatorGuard::acquire();
    unsafe{MemoryStats{total_usable_bytes,free_frames:FREE_FRAMES,initialized:READY}}
}
