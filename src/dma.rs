//! DMA-safe physically contiguous buffers for 32-bit bus masters.
use core::ptr::write_bytes;
use core::sync::atomic::{compiler_fence,AtomicU8,Ordering};

const DMA_CPU_OWNED:u8=0;
const DMA_DEVICE_OWNED:u8=1;
const DMA_QUARANTINED:u8=2;

#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum DmaOwnership{CpuOwned,DeviceOwned,Quarantined}

/// Owns a contiguous, page-aligned physical allocation. In the current bring-up
/// map, its virtual pointer is the same as its physical address (identity map).
/// Do not access it after release; never allocate/free it from an interrupt handler.
pub struct DmaBuffer{
    physical:u64,
    virtual_address:*mut u8,
    length:usize,
    pages:u32,
    ownership:AtomicU8,
}
impl DmaBuffer{
    pub fn physical_address(&self)->u64{self.physical}
    pub fn virtual_address(&self)->*mut u8{self.virtual_address}
    pub fn length(&self)->usize{self.length}
    pub fn page_count(&self)->u32{self.pages}

    pub fn ownership(&self)->DmaOwnership{
        match self.ownership.load(Ordering::Acquire){
            DMA_DEVICE_OWNED=>DmaOwnership::DeviceOwned,
            DMA_QUARANTINED=>DmaOwnership::Quarantined,
            _=>DmaOwnership::CpuOwned,
        }
    }

    /// Publish initialized DMA memory and transfer ownership to the device.
    pub fn handoff_to_device(&self)->bool{
        compiler_fence(Ordering::Release);
        self.ownership.compare_exchange(DMA_CPU_OWNED,DMA_DEVICE_OWNED,
            Ordering::AcqRel,Ordering::Acquire).is_ok()
    }

    /// Quarantined allocations are intentionally never returned to the allocator.
    pub fn quarantine(&self){
        self.ownership.store(DMA_QUARANTINED,Ordering::Release);
    }

    /// Caller must independently verify controller halt, bus-master disable,
    /// and completion of every outstanding DMA transaction.
    pub unsafe fn reclaim_after_quiescence(&self)->bool{
        self.ownership.compare_exchange(DMA_DEVICE_OWNED,DMA_CPU_OWNED,
            Ordering::AcqRel,Ordering::Acquire).is_ok()
    }

    /// Reclaim a single payload after its matching terminal Transfer Event
    /// has been validated. The caller must prove that the event belongs to the
    /// TD that used this buffer; this is not a controller-wide quiescence API.
    pub unsafe fn reclaim_after_transfer_completion(&self)->bool{
        self.ownership.compare_exchange(DMA_DEVICE_OWNED,DMA_CPU_OWNED,
            Ordering::AcqRel,Ordering::Acquire).is_ok()
    }

    /// Release the allocation. The caller must first ensure the device no longer
    /// owns or can access this buffer.
    pub unsafe fn release(self)->bool{
        if self.ownership.load(Ordering::Acquire)!=DMA_CPU_OWNED{return false;}
        compiler_fence(Ordering::SeqCst);
        crate::memory::free_pages(self.physical as u32,self.pages)
    }
}

/// Allocate a zeroed DMA region below max_address (exclusive).
/// alignment and boundary are byte quantities; alignment must be >= 4 KiB and
/// power-of-two. boundary=0 means no boundary crossing restriction.
pub fn alloc(length:usize,alignment:usize,boundary:usize,max_address:u64)->Option<DmaBuffer>{
    if !crate::paging::is_ready()||length==0||alignment<crate::memory::PAGE_SIZE as usize||!alignment.is_power_of_two(){return None;}
    if boundary!=0&&(boundary<crate::memory::PAGE_SIZE as usize||!boundary.is_power_of_two()){return None;}
    let rounded=length.checked_add(crate::memory::PAGE_SIZE as usize-1)?;
    let pages_usize=rounded/(crate::memory::PAGE_SIZE as usize);
    let pages=u32::try_from(pages_usize).ok()?;
    let alignment_pages=u32::try_from(alignment/(crate::memory::PAGE_SIZE as usize)).ok()?;
    let boundary_bytes=boundary as u64;
    let physical=crate::memory::alloc_pages_aligned(pages,alignment_pages,max_address,boundary_bytes)?;
    let pointer=physical as usize as *mut u8;
    unsafe{write_bytes(pointer,0,pages as usize*crate::memory::PAGE_SIZE as usize);}
    compiler_fence(Ordering::SeqCst);
    Some(DmaBuffer{physical:physical as u64,virtual_address:pointer,length,pages,ownership:AtomicU8::new(DMA_CPU_OWNED)})
}
