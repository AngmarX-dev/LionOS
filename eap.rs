//! Page-backed global allocator. One allocation owns whole contiguous 4 KiB pages.
use core::alloc::{GlobalAlloc,Layout};
use core::ptr::{null_mut,write_bytes};
use crate::memory::{self,PAGE_SIZE};

pub struct KernelAllocator;
#[global_allocator]
static GLOBAL:KernelAllocator=KernelAllocator;

unsafe impl GlobalAlloc for KernelAllocator{
    unsafe fn alloc(&self,layout:Layout)->*mut u8{
        if layout.align()>PAGE_SIZE as usize{return null_mut();}
        let size=layout.size().max(1);
        let Some(rounded)=size.checked_add(PAGE_SIZE as usize-1)else{return null_mut();};
        let pages=(rounded/PAGE_SIZE as usize) as u32;
        match memory::alloc_pages(pages){Some(address)=>address as *mut u8,None=>null_mut()}
    }
    unsafe fn dealloc(&self,pointer:*mut u8,layout:Layout){
        if pointer.is_null()||(pointer as usize)>u32::MAX as usize{return;}
        let size=layout.size().max(1);
        let Some(rounded)=size.checked_add(PAGE_SIZE as usize-1)else{return;};
        let pages=(rounded/PAGE_SIZE as usize) as u32;
        let _=memory::free_pages(pointer as u32,pages);
    }
}
pub fn zeroed_page()->Option<*mut u8>{
    let address=memory::alloc_frame()?;
    unsafe{write_bytes(address as *mut u8,0,PAGE_SIZE as usize);}
    Some(address as *mut u8)
}
