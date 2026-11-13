//! xHCI DMA memory layout. Allocates structures only; never starts USB hardware.
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
const DCBAA_ENTRIES:usize=256;
const MAX_SCRATCHPADS:u16=1023;
#[repr(C,align(16))]
#[derive(Clone,Copy,Default)]
pub struct Trb{pub parameter:u64,pub status:u32,pub control:u32}
const _:[();TRB_BYTES]=[();core::mem::size_of::<Trb>()];
#[repr(C,align(16))]
#[derive(Clone,Copy,Default)]
struct ErstEntry{base:u64,size:u32,reserved:u32}
const _:[();16]=[();core::mem::size_of::<ErstEntry>()];
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum DmaError{AllocationFailed,InvalidAlignment,InvalidAddress,InvalidContextSize,TooManyScratchpads}
fn alloc_region(length:usize,boundary:usize)->Option<crate::dma::DmaBuffer>{
 crate::dma::alloc(length,PAGE,boundary,DMA_LIMIT)
}
fn valid_ring(address:u64)->bool{
 address!=0&&address&0x3F==0&&address.checked_add(RING_BYTES as u64)
 .map_or(false,|end|end<=DMA_LIMIT&&(address>>16)==((end-1)>>16))
}
fn valid_page(buffer:&crate::dma::DmaBuffer,length:usize)->bool{
 buffer.physical_address()!=0&&buffer.physical_address()&0xFFF==0
 &&buffer.physical_address().checked_add(length as u64).map_or(false,|end|end<=DMA_LIMIT)
 &&buffer.length()>=length
}
unsafe fn trb_ptr(buffer:&crate::dma::DmaBuffer,index:usize)->*mut Trb{
 (buffer.virtual_address() as *mut Trb).add(index)
}
fn init_link(buffer:&crate::dma::DmaBuffer)->bool{
 let base=buffer.physical_address();if !valid_ring(base){return false;}
 let link=Trb{parameter:base,status:0,control:(TRB_TYPE_LINK<<TRB_TYPE_SHIFT)|TRB_TOGGLE_CYCLE|TRB_CYCLE};
 unsafe{write_volatile(trb_ptr(buffer,LINK_INDEX),link);}
 compiler_fence(Ordering::SeqCst);true
}
pub struct ProducerRing{buffer:crate::dma::DmaBuffer,enqueue:usize,cycle:bool}
impl ProducerRing{
 fn new(buffer:crate::dma::DmaBuffer)->Self{let _=init_link(&buffer);Self{buffer,enqueue:0,cycle:true}}
 pub fn physical_base(&self)->u64{self.buffer.physical_address()}
 pub fn enqueue_index(&self)->usize{self.enqueue}
 pub fn cycle_state(&self)->bool{self.cycle}
 pub fn push(&mut self,mut trb:Trb)->bool{
  if self.enqueue>=LINK_INDEX{return false;}
  trb.control=(trb.control&!TRB_CYCLE)|if self.cycle{TRB_CYCLE}else{0};
  unsafe{write_volatile(trb_ptr(&self.buffer,self.enqueue),trb);}
  compiler_fence(Ordering::Release);self.enqueue+=1;
  if self.enqueue==LINK_INDEX{
   let link=Trb{parameter:self.buffer.physical_address(),status:0,
    control:(TRB_TYPE_LINK<<TRB_TYPE_SHIFT)|TRB_TOGGLE_CYCLE|if self.cycle{TRB_CYCLE}else{0}};
   unsafe{write_volatile(trb_ptr(&self.buffer,LINK_INDEX),link);}
   compiler_fence(Ordering::SeqCst);self.enqueue=0;self.cycle=!self.cycle;
  }
  true
 }
}
pub struct EventRing{buffer:crate::dma::DmaBuffer,erst:crate::dma::DmaBuffer,dequeue:usize,cycle:bool}
impl EventRing{
 fn new(buffer:crate::dma::DmaBuffer,erst:crate::dma::DmaBuffer)->Self{
  let entry=ErstEntry{base:buffer.physical_address(),size:RING_TRBS as u32,reserved:0};
  unsafe{write_volatile(erst.virtual_address() as *mut ErstEntry,entry);}
  compiler_fence(Ordering::SeqCst);Self{buffer,erst,dequeue:0,cycle:true}
 }
 pub fn erst_base(&self)->u64{self.erst.physical_address()}
 pub fn erst_size(&self)->u16{1}
 pub fn segment_base(&self)->u64{self.buffer.physical_address()}
 pub fn segment_trb_count(&self)->u16{RING_TRBS as u16}
 pub fn dequeue_index(&self)->usize{self.dequeue}
 pub fn cycle_state(&self)->bool{self.cycle}
 pub fn dequeue_pointer(&self)->u64{self.buffer.physical_address()+(self.dequeue*TRB_BYTES) as u64}
 pub fn peek(&self)->Option<Trb>{
  compiler_fence(Ordering::Acquire);
  let trb=unsafe{read_volatile(trb_ptr(&self.buffer,self.dequeue))};
  if (trb.control&TRB_CYCLE!=0)!=self.cycle{return None;}Some(trb)
 }
 pub fn consume(&mut self)->bool{
  if self.peek().is_none(){return false;}self.dequeue+=1;
  if self.dequeue==RING_TRBS{self.dequeue=0;self.cycle=!self.cycle;}true
 }
 /// Consume one cycle-valid event and return a decoded copy. This does not
 /// acknowledge ERDP.EHB or write ERDP; the caller must do that via MMIO.
 pub fn next_event(&mut self)->Option<EventRecord>{
  let trb=self.peek()?;
  let raw_type=((trb.control>>TRB_TYPE_SHIFT)&0x3F) as u8;
  let record=EventRecord{trb,event_type:EventType::from_trb_type(raw_type),
   completion_code:(trb.status>>24) as u8,endpoint_id:((trb.control>>16)&0x1F) as u8,
   slot_id:(trb.control>>24) as u8};
  if !self.consume(){return None;}Some(record)
 }
}
pub struct DeviceContext{device:crate::dma::DmaBuffer,input:crate::dma::DmaBuffer,context_size:usize}
impl DeviceContext{
 fn new(device:crate::dma::DmaBuffer,input:crate::dma::DmaBuffer,context_size:usize)->Self{Self{device,input,context_size}}
 pub fn device_base(&self)->u64{self.device.physical_address()}
 pub fn input_base(&self)->u64{self.input.physical_address()}
 pub fn context_size(&self)->usize{self.context_size}
 pub fn device_context_bytes(&self)->usize{32*self.context_size}
 pub fn input_context_bytes(&self)->usize{33*self.context_size}
}
pub struct ScratchpadSet{array:crate::dma::DmaBuffer,storage:crate::dma::DmaBuffer,count:u16}
impl ScratchpadSet{
 fn new(array:crate::dma::DmaBuffer,storage:crate::dma::DmaBuffer,count:u16)->Self{
  let ptr=array.virtual_address() as *mut u64;
  for i in 0..count as usize{unsafe{write_volatile(ptr.add(i),storage.physical_address()+(i*PAGE) as u64);}}
  compiler_fence(Ordering::SeqCst);Self{array,storage,count}
 }
 pub fn array_base(&self)->u64{self.array.physical_address()}
 pub fn storage_base(&self)->u64{self.storage.physical_address()}
 pub fn count(&self)->u16{self.count}
}
pub struct XhciDma{
 dcbaa:crate::dma::DmaBuffer,
 pub command_ring:ProducerRing,
 pub transfer_ring:ProducerRing,
 pub event_ring:EventRing,
 pub device_context:DeviceContext,
 pub scratchpads:Option<ScratchpadSet>,
}
impl XhciDma{
 pub fn allocate(scratch_count:u16,context_size:usize)->Result<Self,DmaError>{
  if scratch_count>MAX_SCRATCHPADS{return Err(DmaError::TooManyScratchpads);}
  if context_size!=32&&context_size!=64{return Err(DmaError::InvalidContextSize);}
  let array_len=(scratch_count as usize).checked_mul(8).ok_or(DmaError::AllocationFailed)?.max(8);
  let storage_len=(scratch_count as usize).checked_mul(PAGE).ok_or(DmaError::AllocationFailed)?.max(PAGE);
  let device_len=32usize.checked_mul(context_size).ok_or(DmaError::AllocationFailed)?;
  let input_len=33usize.checked_mul(context_size).ok_or(DmaError::AllocationFailed)?;
  let dcbaa=alloc_region(DCBAA_ENTRIES*8,0).ok_or(DmaError::AllocationFailed)?;
  let command=match alloc_region(RING_BYTES,65536){Some(v)=>v,None=>{unsafe{let _=dcbaa.release();}return Err(DmaError::AllocationFailed);}};
  let transfer=match alloc_region(RING_BYTES,65536){Some(v)=>v,None=>{unsafe{let _=command.release();let _=dcbaa.release();}return Err(DmaError::AllocationFailed);}};
  let event=match alloc_region(RING_BYTES,65536){Some(v)=>v,None=>{unsafe{let _=transfer.release();let _=command.release();let _=dcbaa.release();}return Err(DmaError::AllocationFailed);}};
  let erst=match alloc_region(16,0){Some(v)=>v,None=>{unsafe{let _=event.release();let _=transfer.release();let _=command.release();let _=dcbaa.release();}return Err(DmaError::AllocationFailed);}};
  let device=match alloc_region(device_len,0){Some(v)=>v,None=>{unsafe{let _=erst.release();let _=event.release();let _=transfer.release();let _=command.release();let _=dcbaa.release();}return Err(DmaError::AllocationFailed);}};
  let input=match alloc_region(input_len,0){Some(v)=>v,None=>{unsafe{let _=device.release();let _=erst.release();let _=event.release();let _=transfer.release();let _=command.release();let _=dcbaa.release();}return Err(DmaError::AllocationFailed);}};
  let scratch_array=if scratch_count>0{match alloc_region(array_len,0){Some(v)=>Some(v),None=>{unsafe{let _=input.release();let _=device.release();let _=erst.release();let _=event.release();let _=transfer.release();let _=command.release();let _=dcbaa.release();}return Err(DmaError::AllocationFailed);}}}else{None};
  let scratch_storage=if scratch_count>0{match alloc_region(storage_len,0){Some(v)=>Some(v),None=>{unsafe{if let Some(v)=scratch_array{let _=v.release();}let _=input.release();let _=device.release();let _=erst.release();let _=event.release();let _=transfer.release();let _=command.release();let _=dcbaa.release();}return Err(DmaError::AllocationFailed);}}}else{None};
  let valid=valid_page(&dcbaa,DCBAA_ENTRIES*8)&&valid_ring(command.physical_address())
   &&valid_ring(transfer.physical_address())&&valid_ring(event.physical_address())
   &&valid_page(&erst,16)&&valid_page(&device,device_len)&&valid_page(&input,input_len)
   &&scratch_array.as_ref().map_or(true,|v|valid_page(v,array_len))
   &&scratch_storage.as_ref().map_or(true,|v|valid_page(v,storage_len));
  if !valid{
   unsafe{if let Some(v)=scratch_storage{let _=v.release();}if let Some(v)=scratch_array{let _=v.release();}
    let _=input.release();let _=device.release();let _=erst.release();let _=event.release();let _=transfer.release();let _=command.release();let _=dcbaa.release();}
   return Err(DmaError::InvalidAlignment);
  }
  let scratchpads=match (scratch_array,scratch_storage){
   (Some(a),Some(s))=>Some(ScratchpadSet::new(a,s,scratch_count)),(None,None)=>None,
   _=>return Err(DmaError::InvalidAddress),
  };
  let dcbaa_ptr=dcbaa.virtual_address() as *mut u64;
  if let Some(ref pads)=scratchpads{unsafe{write_volatile(dcbaa_ptr,pads.array_base());}}
  unsafe{write_volatile(dcbaa_ptr.add(1),device.physical_address());}
  compiler_fence(Ordering::SeqCst);
  Ok(Self{dcbaa,command_ring:ProducerRing::new(command),transfer_ring:ProducerRing::new(transfer),
   event_ring:EventRing::new(event,erst),device_context:DeviceContext::new(device,input,context_size),scratchpads})
 }
 pub fn dcbaa_base(&self)->u64{self.dcbaa.physical_address()}
 pub fn dcbaa_entries(&self)->u16{DCBAA_ENTRIES as u16}
 pub fn command_ring_base(&self)->u64{self.command_ring.physical_base()}
 pub fn scratchpad_count(&self)->u16{self.scratchpads.as_ref().map_or(0,|v|v.count())}
}
