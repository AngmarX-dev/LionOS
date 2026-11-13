//! Identity-mapped, uncached PCI MMIO windows for the current 32-bit PSE map.
use core::ptr::{read_volatile,write_volatile};
use core::sync::atomic::{compiler_fence,Ordering};

pub struct MmioRegion{base:u32,length:usize}
impl MmioRegion{
    pub fn physical_base(&self)->u32{self.base}
    pub fn length(&self)->usize{self.length}

    /// Map a memory BAR within the current 4 GiB identity map.
    /// BAR sizing/resource assignment and 64-bit addresses above 4 GiB are not supported.
    pub unsafe fn from_pci_bar(device:&crate::pci::PciDevice,index:usize,length:usize)->Option<Self>{
        if index>=crate::pci::PCI_BAR_COUNT||length==0{return None;}
        let bar=device.bars[index];
        if bar.kind!=2&&bar.kind!=4{return None;} // 32-bit or 64-bit memory BAR
        if bar.base==0||bar.base>u32::MAX as u64{return None;}
        let end=bar.base.checked_add(length as u64)?;
        if end>0x1_0000_0000||bar.base&3!=0{return None;}
        if !crate::paging::mark_mmio_uncacheable(bar.base,length){return None;}
        if !crate::pci::enable_memory_space(device){return None;}
        compiler_fence(Ordering::SeqCst);
        Some(Self{base:bar.base as u32,length})
    }

    fn valid_offset(&self,offset:usize)->bool{
        offset&3==0&&offset.checked_add(4).map_or(false,|end|end<=self.length)
    }
    pub fn read32(&self,offset:usize)->Option<u32>{
        if !self.valid_offset(offset){return None;}
        compiler_fence(Ordering::SeqCst);
        let value=unsafe{read_volatile((self.base as usize as *const u32).add(offset/4))};
        compiler_fence(Ordering::SeqCst);
        Some(value)
    }
    pub fn write32(&self,offset:usize,value:u32)->bool{
        if !self.valid_offset(offset){return false;}
        compiler_fence(Ordering::SeqCst);
        unsafe{write_volatile((self.base as usize as *mut u32).add(offset/4),value);}
        compiler_fence(Ordering::SeqCst);
        true
    }
}
