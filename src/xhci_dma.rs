//! xHCI DMA structures: DCBAA, producer rings, event ring and ERST.
//! Memory preparation only: no controller register writes or USB transfers.
use core::ptr::{read_volatile,write_volatile};
use core::sync::atomic::{compiler_fence,Ordering};
const DMA_LIMIT:u64=0x1_0000_0000;
const PAGE:usize=4096;
const RING_TRBS:usize=256;
const LINK_INDEX:usize=RING_TRBS-1;
const TRB_BYTES:usize=16;
const RING_BYTES:usize=RING_TRBS*TRB_BYTES;
const TRB_TYPE_LINK:u32=6;
const TRB_CYCLE:u32=1;
const TRB_TOGGLE_CYCLE:u32=1<<1;
const TRB_TYPE_SHIFT:u32=10;
#[repr(C,align(16))]
#[derive(Clone,Copy,Default)]
pub struct Trb{pub parameter:u64,pub status:u32,pub control:u32}
const _: [();TRB_BYTES]=[();core::mem::size_of::<Trb>()];
#[repr(C,align(16))]
#[derive(Clone,Copy,Default)]
struct ErstEntry{base:u64,size:u32,reserved:u32}
const _: [();16]=[();core::mem::size_of::<ErstEntry>()];
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum DmaError{UnsupportedScratchpads,AllocationFailed,InvalidAlignment,InvalidAddress}
fn allocate_region()->Option<crate::dma::DmaBuffer>{
    crate::dma::alloc(PAGE,PAGE,65536,DMA_LIMIT)
}
fn valid_ring_address(address:u64)->bool{
    address!=0&&address&0x3F==0&&address.checked_add(RING_BYTES as u64)
        .map_or(false,|end|end<=DMA_LIMIT&&((address>>16)==((end-1)>>16)))
}
unsafe fn trb_ptr(buffer:&crate::dma::DmaBuffer,index:usize)->*mut Trb{
    (buffer.virtual_address() as *mut Trb).add(index)
}
fn initialize_link(buffer:&crate::dma::DmaBuffer)->bool{
    let base=buffer.physical_address();
    if !valid_ring_address(base){return false;}
    let link=Trb{parameter:base,status:0,
        control:(TRB_TYPE_LINK<<TRB_TYPE_SHIFT)|TRB_TOGGLE_CYCLE|TRB_CYCLE};
    unsafe{write_volatile(trb_ptr(buffer,LINK_INDEX),link);}
    compiler_fence(Ordering::SeqCst);
    true
}
/// Producer ring with a terminal Link TRB. push() never notifies hardware.
pub struct ProducerRing{buffer:crate::dma::DmaBuffer,enqueue:usize,cycle:bool}
impl ProducerRing{
    fn new(buffer:crate::dma::DmaBuffer)->Option<Self>{
        if !initialize_link(&buffer){return None;}
        Some(Self{buffer,enqueue:0,cycle:true})
    }
    pub fn physical_base(&self)->u64{self.buffer.physical_address()}
    pub fn enqueue_index(&self)->usize{self.enqueue}
    pub fn cycle_state(&self)->bool{self.cycle}
    pub fn push(&mut self,mut trb:Trb)->bool{
        if self.enqueue>=LINK_INDEX{return false;}
        trb.control=(trb.control&!TRB_CYCLE)|if self.cycle{TRB_CYCLE}else{0};
        unsafe{write_volatile(trb_ptr(&self.buffer,self.enqueue),trb);}
        compiler_fence(Ordering::Release);
        self.enqueue+=1;
        if self.enqueue==LINK_INDEX{
            self.enqueue=0;self.cycle=!self.cycle;
            let link=Trb{parameter:self.buffer.physical_address(),status:0,
                control:(TRB_TYPE_LINK<<TRB_TYPE_SHIFT)|TRB_TOGGLE_CYCLE|if self.cycle{TRB_CYCLE}else{0}};
            unsafe{write_volatile(trb_ptr(&self.buffer,LINK_INDEX),link);}
            compiler_fence(Ordering::SeqCst);
        }
        true
    }
}
/// Single-segment Event Ring and its Event Ring Segment Table.
pub struct EventRing{buffer:crate::dma::DmaBuffer,erst:crate::dma::DmaBuffer,dequeue:usize,cycle:bool}
impl EventRing{
    fn new(buffer:crate::dma::DmaBuffer,erst:crate::dma::DmaBuffer)->Option<Self>{
        if !valid_ring_address(buffer.physical_address())
            ||erst.physical_address()==0||erst.physical_address()&0x3F!=0{return None;}
        let entry=ErstEntry{base:buffer.physical_address(),size:RING_TRBS as u32,reserved:0};
        unsafe{write_volatile(erst.virtual_address() as *mut ErstEntry,entry);}
        compiler_fence(Ordering::SeqCst);
        Some(Self{buffer,erst,dequeue:0,cycle:true})
    }
    pub fn erst_base(&self)->u64{self.erst.physical_address()}
    pub fn erst_size(&self)->u16{1}
    pub fn segment_base(&self)->u64{self.buffer.physical_address()}
    pub fn segment_trb_count(&self)->u16{RING_TRBS as u16}
    pub fn dequeue_index(&self)->usize{self.dequeue}
    pub fn cycle_state(&self)->bool{self.cycle}
    pub fn peek(&self)->Option<Trb>{
        let trb=unsafe{read_volatile(trb_ptr(&self.buffer,self.dequeue))};
        if (trb.control&TRB_CYCLE!=0)!=self.cycle{return None;}
        compiler_fence(Ordering::Acquire);Some(trb)
    }
    pub fn consume(&mut self)->bool{
        if self.peek().is_none(){return false;}
        self.dequeue+=1;
        if self.dequeue==RING_TRBS{self.dequeue=0;self.cycle=!self.cycle;}
        true
    }
}
/// Initial single-interrupter, single-segment memory layout.
/// DCBAA entries 0..=255 are zeroed; scratchpad arrays are not yet supported.
pub struct XhciDma{
    dcbaa:crate::dma::DmaBuffer,
    pub command_ring:ProducerRing,
    pub transfer_ring:ProducerRing,
    pub event_ring:EventRing,
}
impl XhciDma{
    pub fn allocate(scratchpad_count:u16)->Result<Self,DmaError>{
        if scratchpad_count!=0{return Err(DmaError::UnsupportedScratchpads);}
        let dcbaa=allocate_region().ok_or(DmaError::AllocationFailed)?;
        let command=match allocate_region(){Some(v)=>v,None=>{unsafe{let _=dcbaa.release();}return Err(DmaError::AllocationFailed);}};
        let transfer=match allocate_region(){Some(v)=>v,None=>{unsafe{let _=command.release();let _=dcbaa.release();}return Err(DmaError::AllocationFailed);}};
        let event=match allocate_region(){Some(v)=>v,None=>{unsafe{let _=transfer.release();let _=command.release();let _=dcbaa.release();}return Err(DmaError::AllocationFailed);}};
        let erst=match allocate_region(){Some(v)=>v,None=>{unsafe{let _=event.release();let _=transfer.release();let _=command.release();let _=dcbaa.release();}return Err(DmaError::AllocationFailed);}};
        if dcbaa.physical_address()&0x3F!=0||dcbaa.length()<256*8{
            unsafe{let _=erst.release();let _=event.release();let _=transfer.release();let _=command.release();let _=dcbaa.release();}
            return Err(DmaError::InvalidAlignment);
        }
        for address in [dcbaa.physical_address(),command.physical_address(),transfer.physical_address(),event.physical_address(),erst.physical_address()]{
            if address==0||address>=DMA_LIMIT{
                unsafe{let _=erst.release();let _=event.release();let _=transfer.release();let _=command.release();let _=dcbaa.release();}
                return Err(DmaError::InvalidAddress);
            }
        }
        let command_ring=match ProducerRing::new(command){
            Some(v)=>v,None=>{unsafe{let _=erst.release();let _=event.release();let _=transfer.release();let _=dcbaa.release();}return Err(DmaError::InvalidAlignment);}
        };
        let transfer_ring=match ProducerRing::new(transfer){
            Some(v)=>v,None=>{unsafe{let _=erst.release();let _=event.release();let _=dcbaa.release();}return Err(DmaError::InvalidAlignment);}
        };
        let event_ring=match EventRing::new(event,erst){
            Some(v)=>v,None=>return Err(DmaError::InvalidAlignment)
        };
        Ok(Self{dcbaa,command_ring,transfer_ring,event_ring})
    }
    pub fn dcbaa_base(&self)->u64{self.dcbaa.physical_address()}
    pub fn dcbaa_entries(&self)->u16{256}
    pub fn command_ring_base(&self)->u64{self.command_ring.physical_base()}
}
