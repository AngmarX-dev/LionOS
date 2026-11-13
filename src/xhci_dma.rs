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
pub struct EventRing{
 buffer:crate::dma::DmaBuffer,erst:crate::dma::DmaBuffer,
 dequeue:usize,cycle:bool,consumed:u64,
}
impl EventRing{
 fn new(buffer:crate::dma::DmaBuffer,erst:crate::dma::DmaBuffer)->Self{
  let entry=ErstEntry{base:buffer.physical_address(),size:RING_TRBS as u32,reserved:0};
  unsafe{write_volatile(erst.virtual_address() as *mut ErstEntry,entry);}
  compiler_fence(Ordering::SeqCst);
  Self{buffer,erst,dequeue:0,cycle:true,consumed:0}
 }
 pub fn erst_base(&self)->u64{self.erst.physical_address()}
 pub fn erst_size(&self)->u16{1}
 pub fn segment_base(&self)->u64{self.buffer.physical_address()}
 pub fn segment_trb_count(&self)->u16{RING_TRBS as u16}
 pub fn dequeue_index(&self)->usize{self.dequeue}
 pub fn cycle_state(&self)->bool{self.cycle}
 pub fn consumed_count(&self)->u64{self.consumed}
 pub fn dequeue_pointer(&self)->u64{
  self.buffer.physical_address()+(self.dequeue*TRB_BYTES) as u64
 }
 /// Value to write to ERDP after consuming events. Bit 3 requests EHB
 /// acknowledgement; low pointer bits remain naturally aligned.
 pub fn erdp_ack_value(&self)->u64{self.dequeue_pointer()|(1<<3)}
 pub fn peek(&self)->Option<Trb>{
  compiler_fence(Ordering::Acquire);
  let trb=unsafe{read_volatile(trb_ptr(&self.buffer,self.dequeue))};
  if (trb.control&TRB_CYCLE!=0)!=self.cycle{return None;}Some(trb)
 }
 pub fn consume(&mut self)->bool{
  if self.peek().is_none(){return false;}
  self.dequeue+=1;self.consumed=self.consumed.wrapping_add(1);
  if self.dequeue==RING_TRBS{self.dequeue=0;self.cycle=!self.cycle;}
  true
 }
 /// Consume at most max_events, returning decoded records to the caller.
 /// Hardware ERDP acknowledgement is intentionally a separate operation.
 pub fn drain(&mut self,max_events:usize,out:&mut [Option<EventRecord>])->usize{
  let limit=core::cmp::min(max_events,out.len());let mut count=0;
  while count<limit{
   let trb=match self.peek(){Some(v)=>v,None=>break};
   let raw_type=((trb.control>>TRB_TYPE_SHIFT)&0x3F) as u8;
   out[count]=Some(EventRecord{trb,event_type:EventType::from_trb_type(raw_type),
    completion_code:(trb.status>>24) as u8,endpoint_id:((trb.control>>16)&0x1F) as u8,
    slot_id:(trb.control>>24) as u8});
   if !self.consume(){out[count]=None;break;}
   count+=1;
  }
  count
 }
 /// Consume one cycle-valid event and return a decoded copy.
 pub fn next_event(&mut self)->Option<EventRecord>{
  let mut one=[None];if self.drain(1,&mut one)==1{one[0]}else{None}
 }
}

/// Decoded xHCI event TRB category.
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum EventType {
    Transfer,
    CommandCompletion,
    PortStatusChange,
    BandwidthRequest,
    Doorbell,
    HostController,
    DeviceNotification,
    MfindexWrap,
    Unknown,
}
impl EventType {
    pub const fn from_trb_type(raw:u8)->Self {
        match raw {
            32=>Self::Transfer,
            33=>Self::CommandCompletion,
            34=>Self::PortStatusChange,
            35=>Self::BandwidthRequest,
            36=>Self::Doorbell,
            37=>Self::HostController,
            38=>Self::DeviceNotification,
            39=>Self::MfindexWrap,
            _=>Self::Unknown,
        }
    }
}
#[derive(Clone,Copy)]
pub struct EventRecord {
    pub trb:Trb,
    pub event_type:EventType,
    pub completion_code:u8,
    pub endpoint_id:u8,
    pub slot_id:u8,
}
impl EventRecord {
    /// Command Completion Event parameter contains the completed command TRB
    /// address; low four bits are reserved and excluded from matching.
    pub fn command_trb_pointer(&self)->Option<u64> {
        if self.event_type!=EventType::CommandCompletion{return None;}
        Some(self.trb.parameter&!0xF)
    }
    pub fn completed_successfully(&self)->bool { self.completion_code==1 }
}

pub struct DeviceContext{device:crate::dma::DmaBuffer,input:crate::dma::DmaBuffer,context_size:usize}
impl DeviceContext{
 fn new(device:crate::dma::DmaBuffer,input:crate::dma::DmaBuffer,context_size:usize)->Self{Self{device,input,context_size}}
 pub fn device_base(&self)->u64{self.device.physical_address()}
 pub fn input_base(&self)->u64{self.input.physical_address()}
 pub fn context_size(&self)->usize{self.context_size}
 pub fn device_context_bytes(&self)->usize{32*self.context_size}
 pub fn input_context_bytes(&self)->usize{33*self.context_size}
 /// Write one dword in an Input Context (context 0..32). The backing DMA
 /// allocation is zero-initialized and remains owned by this object.
 pub fn set_input_dword(&mut self,context_index:usize,dword_index:usize,value:u32)->bool{
  if context_index>=33||dword_index>=self.context_size/4{return false;}
  let offset=match context_index.checked_mul(self.context_size)
   .and_then(|v|v.checked_add(dword_index*4)){Some(v)=>v,None=>return false};
  if offset.checked_add(4).map_or(true,|end|end>self.input.length()){return false;}
  let ptr=(self.input.virtual_address() as *mut u32).wrapping_add(offset/4);
  unsafe{write_volatile(ptr,value);}
  compiler_fence(Ordering::Release);true
 }
 /// Prepare the Input Control Context, Slot Context and default control
 /// endpoint context for Address Device. Caller supplies descriptor-derived
 /// speed/max-packet information and a valid EP0 transfer-ring dequeue pointer.
 pub fn prepare_address_context(&mut self,route_string:u32,speed:u8,root_port:u8,
  context_entries:u8,ep0_max_packet:u16,ep0_dequeue:u64,cycle_state:bool)->bool{
  if route_string>0xFFFFF||speed==0||speed>15||root_port==0
   ||context_entries<1||context_entries>31
   ||ep0_max_packet==0||ep0_dequeue==0||ep0_dequeue&0xF!=0
   ||ep0_dequeue>=0x1_0000_0000{return false;}
  let add_context=(1u32<<0)|(1u32<<1);
  let slot_dword0=route_string|((speed as u32)<<20)|((context_entries as u32)<<27);
  let slot_dword1=(root_port as u32)<<16;
  let ep0_dword1=(3u32<<1)|(4u32<<3)|((ep0_max_packet as u32)<<16);
  let ep0_dword2=(ep0_dequeue as u32)&!0xF;
  let ep0_dword3=((ep0_dequeue>>32) as u32)|if cycle_state{1}else{0};
  self.set_input_dword(0,1,add_context)
   &&self.set_input_dword(1,0,slot_dword0)
   &&self.set_input_dword(1,1,slot_dword1)
   &&self.set_input_dword(2,1,ep0_dword1)
   &&self.set_input_dword(2,2,ep0_dword2)
   &&self.set_input_dword(2,3,ep0_dword3)
   &&self.set_input_dword(2,4,8)
 }
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


/// A command submitted to the command ring and awaiting its completion event.
#[derive(Clone,Copy)]
struct PendingCommand {
    trb_pointer:u64,
    deadline:u64,
    timed_out:bool,
}

/// Bounded software-side correlation table for Command Completion Events.
/// It deliberately does not ring the xHCI doorbell or touch controller state.
pub struct CommandTracker {
    pending:[Option<PendingCommand>;254],
    len:usize,
}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum CommandError {
    Full,
    RingUnavailable,
    NotCommandCompletion,
    UnknownCommand,
    DuplicatePointer,
}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub struct CommandResult {
    pub trb_pointer:u64,
    pub completion_code:u8,
    pub slot_id:u8,
}
impl CommandTracker {
    pub const fn new()->Self {
        Self{pending:[None;254],len:0}
    }
    pub fn len(&self)->usize { self.len }
    pub fn is_empty(&self)->bool { self.len==0 }
    pub fn is_full(&self)->bool { self.len==254 }
    /// Reserve tracking capacity and enqueue a command TRB. Returns its DMA
    /// address so a later Command Completion Event can be matched exactly.
    pub fn submit(&mut self,ring:&mut ProducerRing,trb:Trb)->Result<u64,CommandError> {
        if self.is_full(){return Err(CommandError::Full);}
        let index=ring.enqueue_index();
        if index>=LINK_INDEX{return Err(CommandError::RingUnavailable);}
        let pointer=ring.physical_base()+(index*TRB_BYTES) as u64;
        if self.pending[..self.len].iter().flatten().any(|p|p.trb_pointer==pointer) {
            return Err(CommandError::DuplicatePointer);
        }
        if !ring.push(trb){return Err(CommandError::RingUnavailable);}
        self.pending[self.len]=Some(PendingCommand{trb_pointer:pointer,deadline:u64::MAX,timed_out:false});
        self.len+=1;
        Ok(pointer)
    }
    /// Submit with an absolute deadline expressed in monotonic PIT ticks.
    /// timeout_ticks must be nonzero; overflow saturates to the maximum deadline.
    pub fn submit_with_timeout(&mut self,ring:&mut ProducerRing,trb:Trb,now:u64,timeout_ticks:u64)->Result<u64,CommandError>{
        if timeout_ticks==0{return Err(CommandError::RingUnavailable);}
        let pointer=self.submit(ring,trb)?;
        if let Some(p)=self.pending[..self.len].iter_mut().flatten().find(|p|p.trb_pointer==pointer){
            p.deadline=now.saturating_add(timeout_ticks);
        }
        Ok(pointer)
    }
    /// Mark expired commands but retain their DMA ownership records. A timeout
    /// never frees or reuses a TRB while the controller might still reference it.
    pub fn expire(&mut self,now:u64)->usize{
        let mut expired=0;
        for p in self.pending[..self.len].iter_mut().flatten(){
            if !p.timed_out && p.deadline!=u64::MAX && now>=p.deadline {
                p.timed_out=true;expired+=1;
            }
        }
        expired
    }
    pub fn has_timed_out(&self)->bool{
        self.pending[..self.len].iter().flatten().any(|p|p.timed_out)
    }
    pub fn oldest_timed_out_pointer(&self)->Option<u64>{
        self.pending[..self.len].iter().flatten().find(|p|p.timed_out).map(|p|p.trb_pointer)
    }
    /// Match a decoded completion against the outstanding command table.
    /// Unknown, stale or duplicate completions are rejected without mutation.
    pub fn complete(&mut self,event:&EventRecord)->Result<CommandResult,CommandError> {
        if event.event_type!=EventType::CommandCompletion {
            return Err(CommandError::NotCommandCompletion);
        }
        let pointer=event.command_trb_pointer().ok_or(CommandError::UnknownCommand)?;
        let mut found=None;
        for i in 0..self.len {
            if self.pending[i].map_or(false,|p|p.trb_pointer==pointer) {
                found=Some(i);break;
            }
        }
        let index=found.ok_or(CommandError::UnknownCommand)?;
        let result=CommandResult{trb_pointer:pointer,completion_code:event.completion_code,slot_id:event.slot_id};
        for i in index..self.len-1 { self.pending[i]=self.pending[i+1]; }
        self.len-=1;
        self.pending[self.len]=None;
        Ok(result)
    }
}


/// Software recovery policy states. These are bookkeeping states only;
/// they do not issue Command Abort, reset the controller, or alter PCI/MMIO.
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum RecoveryState {
    Idle,
    TimeoutDetected,
    AbortRequested,
    AbortInProgress,
    ControllerResetRequired,
    Quarantined,
}
pub struct RecoveryManager { state:RecoveryState }
impl RecoveryManager {
    pub const fn new()->Self { Self{state:RecoveryState::Idle} }
    pub fn state(&self)->RecoveryState { self.state }
    pub fn observe_timeout(&mut self,tracker:&CommandTracker)->RecoveryState {
        if tracker.has_timed_out() && self.state==RecoveryState::Idle {
            self.state=RecoveryState::TimeoutDetected;
        }
        self.state
    }
    /// Caller may request an abort only after validating controller state and
    /// ensuring no concurrent command/event consumer is mutating the rings.
    pub fn request_abort(&mut self)->bool {
        if self.state!=RecoveryState::TimeoutDetected{return false;}
        self.state=RecoveryState::AbortRequested;true
    }
    /// Record an externally observed abort-in-progress status.
    pub fn abort_started(&mut self)->bool {
        if self.state!=RecoveryState::AbortRequested{return false;}
        self.state=RecoveryState::AbortInProgress;true
    }
    /// If abort fails or its bounded wait expires, require a full controller
    /// reset. DMA allocations must remain pinned until reset/halt is confirmed.
    pub fn abort_failed(&mut self)->bool {
        if self.state!=RecoveryState::AbortRequested&&self.state!=RecoveryState::AbortInProgress{return false;}
        self.state=RecoveryState::ControllerResetRequired;true
    }
    /// A confirmed controller reset/halt permits the caller to begin a
    /// separately validated reinitialization sequence.
    pub fn reset_confirmed(&mut self)->bool {
        if self.state!=RecoveryState::ControllerResetRequired{return false;}
        self.state=RecoveryState::Quarantined;true
    }
}


/// Result of consuming one event through the software command pipeline.
#[derive(Clone,Copy)]
pub enum CommandPoll {
    Empty,
    Unrelated(EventRecord),
    Completed(CommandResult),
    InvalidCompletion(CommandError),
}
/// Coordinates ring enqueue, timeout bookkeeping and completion correlation.
/// It intentionally does not ring doorbells or enable the controller.
pub struct CommandEngine {
    tracker:CommandTracker,
}
impl CommandEngine {
    pub const fn new()->Self { Self{tracker:CommandTracker::new()} }
    pub fn pending_count(&self)->usize { self.tracker.len() }
    pub fn submit(&mut self,ring:&mut ProducerRing,trb:Trb,now:u64,timeout_ticks:u64)->Result<u64,CommandError> {
        self.tracker.submit_with_timeout(ring,trb,now,timeout_ticks)
    }
    pub fn expire(&mut self,now:u64)->usize { self.tracker.expire(now) }
    pub fn has_timed_out(&self)->bool { self.tracker.has_timed_out() }
    /// Consume at most one cycle-valid event. Non-command events are returned
    /// intact so a higher-level dispatcher can route them instead of dropping.
    pub fn poll_one(&mut self,events:&mut EventRing)->CommandPoll {
        let event=match events.next_event(){Some(v)=>v,None=>return CommandPoll::Empty};
        if event.event_type!=EventType::CommandCompletion {
            return CommandPoll::Unrelated(event);
        }
        match self.tracker.complete(&event) {
            Ok(result)=>CommandPoll::Completed(result),
            Err(error)=>CommandPoll::InvalidCompletion(error),
        }
    }
}


const TRB_TYPE_ENABLE_SLOT:u32=9;
const TRB_TYPE_ADDRESS_DEVICE:u32=11;
/// Construct Enable Slot Command. Slot type 0 asks the controller to choose
/// the default slot type. The returned slot ID is learned from completion.
pub fn enable_slot_command()->Trb{
 Trb{parameter:0,status:0,control:(TRB_TYPE_ENABLE_SLOT<<TRB_TYPE_SHIFT)}
}
/// Construct Address Device Command after the Input Context has been prepared.
/// The slot ID must come from a successful Enable Slot completion.
pub fn address_device_command(input_context:u64,slot_id:u8,block_set_address:bool)->Option<Trb>{
 if input_context==0||input_context&0x3F!=0||input_context>=DMA_LIMIT||slot_id==0{return None;}
 Some(Trb{parameter:input_context,status:0,
  control:(TRB_TYPE_ADDRESS_DEVICE<<TRB_TYPE_SHIFT)|((slot_id as u32)<<24)
   |if block_set_address{1<<9}else{0}})
}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum SlotState { Enabled,AddressPending,Addressed,Failed }
#[derive(Clone,Copy,Debug)]
pub struct SlotRecord { pub slot_id:u8,pub root_port:u8,pub speed:u8,pub state:SlotState }
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum SlotError { InvalidSlot,InvalidPort,InvalidSpeed,AlreadyRegistered,UnknownSlot,InvalidTransition }
/// Bounded software registry; it does not issue Enable Slot or Address Device.
pub struct SlotManager { slots:[Option<SlotRecord>;256],max_slots:u8 }
impl SlotManager {
 pub const fn new(max_slots:u8)->Self { Self{slots:[None;256],max_slots} }
 pub fn register_enabled(&mut self,slot_id:u8,root_port:u8,speed:u8)->Result<(),SlotError>{
  if slot_id==0||slot_id>self.max_slots{return Err(SlotError::InvalidSlot);}
  if root_port==0{return Err(SlotError::InvalidPort);}
  if speed==0||speed>15{return Err(SlotError::InvalidSpeed);}
  if self.slots[slot_id as usize].is_some(){return Err(SlotError::AlreadyRegistered);}
  self.slots[slot_id as usize]=Some(SlotRecord{slot_id,root_port,speed,state:SlotState::Enabled});Ok(())
 }
 pub fn mark_address_pending(&mut self,slot_id:u8)->Result<(),SlotError>{
  let slot=self.slots.get_mut(slot_id as usize).ok_or(SlotError::InvalidSlot)?;
  let record=slot.as_mut().ok_or(SlotError::UnknownSlot)?;
  if record.state!=SlotState::Enabled{return Err(SlotError::InvalidTransition);}
  record.state=SlotState::AddressPending;Ok(())
 }
 pub fn mark_addressed(&mut self,slot_id:u8,success:bool)->Result<(),SlotError>{
  let slot=self.slots.get_mut(slot_id as usize).ok_or(SlotError::InvalidSlot)?;
  let record=slot.as_mut().ok_or(SlotError::UnknownSlot)?;
  if record.state!=SlotState::AddressPending{return Err(SlotError::InvalidTransition);}
  record.state=if success{SlotState::Addressed}else{SlotState::Failed};Ok(())
 }
 pub fn get(&self,slot_id:u8)->Option<SlotRecord>{self.slots.get(slot_id as usize).copied().flatten()}
 pub fn release(&mut self,slot_id:u8)->Result<(),SlotError>{
  let slot=self.slots.get_mut(slot_id as usize).ok_or(SlotError::InvalidSlot)?;
  if slot.is_none(){return Err(SlotError::UnknownSlot);}*slot=None;Ok(())
 }
}


/// USB standard descriptor type constants.
pub const USB_DESC_DEVICE:u8=1;
pub const USB_DESC_CONFIGURATION:u8=2;
pub const USB_DESC_STRING:u8=3;
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum UsbDescriptorError { TooShort,InvalidLength,WrongType,InvalidConfiguration }
/// Parsed USB Device Descriptor. Multi-byte fields are decoded little-endian.
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub struct UsbDeviceDescriptor {
 pub usb_version:u16,pub device_class:u8,pub device_subclass:u8,
 pub device_protocol:u8,pub max_packet_size0:u8,pub vendor_id:u16,
 pub product_id:u16,pub device_version:u16,pub manufacturer_index:u8,
 pub product_index:u8,pub serial_index:u8,pub configuration_count:u8,
}
impl UsbDeviceDescriptor {
 pub const LENGTH:usize=18;
 pub fn parse(bytes:&[u8])->Result<Self,UsbDescriptorError>{
  if bytes.len()<Self::LENGTH{return Err(UsbDescriptorError::TooShort);}
  if bytes[0] as usize!=Self::LENGTH{return Err(UsbDescriptorError::InvalidLength);}
  if bytes[1]!=USB_DESC_DEVICE{return Err(UsbDescriptorError::WrongType);}
  Ok(Self{
   usb_version:u16::from_le_bytes([bytes[2],bytes[3]]),
   device_class:bytes[4],device_subclass:bytes[5],device_protocol:bytes[6],
   max_packet_size0:bytes[7],
   vendor_id:u16::from_le_bytes([bytes[8],bytes[9]]),
   product_id:u16::from_le_bytes([bytes[10],bytes[11]]),
   device_version:u16::from_le_bytes([bytes[12],bytes[13]]),
   manufacturer_index:bytes[14],product_index:bytes[15],
   serial_index:bytes[16],configuration_count:bytes[17],
  })
 }
}
/// Bounds-checked iterator step for a raw USB descriptor stream.
pub fn descriptor_at(bytes:&[u8],offset:usize)->Result<Option<(u8,&[u8])>,UsbDescriptorError>{
 if offset==bytes.len(){return Ok(None);}
 if offset>bytes.len()||bytes.len()-offset<2{return Err(UsbDescriptorError::TooShort);}
 let length=bytes[offset] as usize;
 if length<2{return Err(UsbDescriptorError::InvalidLength);}
 let end=offset.checked_add(length).ok_or(UsbDescriptorError::InvalidLength)?;
 if end>bytes.len(){return Err(UsbDescriptorError::TooShort);}
 Ok(Some((bytes[offset+1],&bytes[offset..end])))
}


/// USB SETUP packet used by control-transfer Setup Stage TRBs.
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub struct UsbSetupPacket {
 pub request_type:u8,pub request:u8,pub value:u16,pub index:u16,pub length:u16,
}
impl UsbSetupPacket {
 pub const fn get_device_descriptor(length:u16)->Self {
  Self{request_type:0x80,request:6,value:0x0100,index:0,length}
 }
 pub const fn to_parameter(self)->u64 {
  u64::from_le_bytes([self.request_type,self.request,
   self.value as u8,(self.value>>8) as u8,
   self.index as u8,(self.index>>8) as u8,
   self.length as u8,(self.length>>8) as u8])
 }
}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum ControlTransferError { InvalidLength,InvalidBuffer,AddressOverflow }
const TRB_TYPE_SETUP_STAGE:u32=2;
const TRB_TYPE_DATA_STAGE:u32=3;
const TRB_TYPE_STATUS_STAGE:u32=4;
const TRB_CHAIN:u32=1<<4;
const TRB_IOC:u32=1<<5;
const TRB_IDT:u32=1<<6;
const TRB_DIR_IN:u32=1<<16;
/// Build the three TRBs for an IN GET_DESCRIPTOR(Device) control TD.
/// This creates software descriptors only; ProducerRing::push is not called.
pub fn get_device_descriptor_control_trbs(data_buffer:u64,length:u16)
 ->Result<[Trb;3],ControlTransferError>{
 if length==0||length>18{return Err(ControlTransferError::InvalidLength);}
 if data_buffer==0||data_buffer>=DMA_LIMIT{return Err(ControlTransferError::InvalidBuffer);}
 let end=data_buffer.checked_add(length as u64).ok_or(ControlTransferError::AddressOverflow)?;
 if end>DMA_LIMIT{return Err(ControlTransferError::AddressOverflow);}
 let setup=UsbSetupPacket::get_device_descriptor(length);
 let setup_trb=Trb{
  parameter:setup.to_parameter(),status:8,
  control:(TRB_TYPE_SETUP_STAGE<<TRB_TYPE_SHIFT)|TRB_IDT|TRB_CHAIN|(3<<16),
 };
 let data_trb=Trb{
  parameter:data_buffer,status:length as u32,
  control:(TRB_TYPE_DATA_STAGE<<TRB_TYPE_SHIFT)|TRB_DIR_IN|TRB_CHAIN,
 };
 let status_trb=Trb{
  parameter:0,status:0,
  control:(TRB_TYPE_STATUS_STAGE<<TRB_TYPE_SHIFT)|TRB_IOC,
 };
 Ok([setup_trb,data_trb,status_trb])
}


/// Completion classification for a USB Transfer Event TRB.
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum TransferCompletion {
    Success,
    ShortPacket,
    Stall,
    TransactionError,
    Babble,
    DataBufferError,
    TrbError,
    RingUnderrun,
    RingOverrun,
    MissedService,
    Stopped,
    Unknown(u8),
}
impl TransferCompletion {
    pub const fn from_code(code:u8)->Self {
        match code {
            1=>Self::Success,
            13=>Self::ShortPacket,
            6=>Self::Stall,
            4=>Self::TransactionError,
            3=>Self::Babble,
            2=>Self::DataBufferError,
            5=>Self::TrbError,
            14=>Self::RingUnderrun,
            15=>Self::RingOverrun,
            23=>Self::MissedService,
            26=>Self::Stopped,
            v=>Self::Unknown(v),
        }
    }
    pub const fn is_success(self)->bool {
        matches!(self,Self::Success|Self::ShortPacket)
    }
}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub struct TransferEvent {
    pub trb_pointer:u64,
    pub completion:TransferCompletion,
    pub residual_length:u32,
    pub endpoint_id:u8,
    pub slot_id:u8,
}
impl TransferEvent {
    pub fn decode(event:&EventRecord)->Option<Self> {
        if event.event_type!=EventType::Transfer{return None;}
        Some(Self{
            trb_pointer:event.trb.parameter&!0xF,
            completion:TransferCompletion::from_code(event.completion_code),
            residual_length:event.trb.status&0x00FF_FFFF,
            endpoint_id:event.endpoint_id,
            slot_id:event.slot_id,
        })
    }
}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum TransferError {
    RingFull,
    TrackerFull,
    InvalidEvent,
    UnknownTransfer,
    InvalidSlot,
    InvalidEndpoint,
    InvalidBuffer,
}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub struct TransferResult {
    pub trb_pointer:u64,
    pub slot_id:u8,
    pub endpoint_id:u8,
    pub completion:TransferCompletion,
    pub residual_length:u32,
}
#[derive(Clone,Copy)]
struct PendingTransfer {
    setup_trb_pointer:u64,
    data_trb_pointer:u64,
    status_trb_pointer:u64,
    slot_id:u8,
    endpoint_id:u8,
}
/// Correlates Transfer Events with the final TRB of each submitted TD.
pub struct TransferTracker {
    pending:[Option<PendingTransfer>;64],
    len:usize,
}
impl TransferTracker {
    pub const fn new()->Self { Self{pending:[None;64],len:0} }
    pub fn pending_count(&self)->usize { self.len }
    fn reserve(&mut self,p:PendingTransfer)->Result<(),TransferError>{
        if self.len==self.pending.len(){return Err(TransferError::TrackerFull);}
        if self.pending[..self.len].iter().flatten().any(|v|
            v.setup_trb_pointer==p.setup_trb_pointer||v.data_trb_pointer==p.data_trb_pointer
             ||v.status_trb_pointer==p.status_trb_pointer){
            return Err(TransferError::UnknownTransfer);
        }
        self.pending[self.len]=Some(p);self.len+=1;Ok(())
    }
    pub fn complete(&mut self,event:&EventRecord)->Result<TransferResult,TransferError>{
        let decoded=TransferEvent::decode(event).ok_or(TransferError::InvalidEvent)?;
        let index=self.pending[..self.len].iter().position(|v|v.map_or(false,|p|
            (p.setup_trb_pointer==decoded.trb_pointer||p.data_trb_pointer==decoded.trb_pointer
             ||p.status_trb_pointer==decoded.trb_pointer)&&p.slot_id==decoded.slot_id
             &&p.endpoint_id==decoded.endpoint_id)).ok_or(TransferError::UnknownTransfer)?;
        let result=TransferResult{trb_pointer:decoded.trb_pointer,slot_id:decoded.slot_id,
            endpoint_id:decoded.endpoint_id,completion:decoded.completion,
            residual_length:decoded.residual_length};
        for i in index..self.len-1 {self.pending[i]=self.pending[i+1];}
        self.len-=1;self.pending[self.len]=None;Ok(result)
    }
}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum TransferPoll { Empty,Unrelated(EventRecord),Completed(TransferResult),Invalid(TransferError) }
/// Conservative one-shot transfer submission coordinator. It deliberately
/// refuses to wrap/reuse the transfer ring: safe reclamation requires a
/// separately verified dequeue/reuse protocol. Only control GET_DESCRIPTOR
/// TDs are supported in this stage.
pub struct TransferEngine {
    tracker:TransferTracker,
    submitted_trbs:usize,
}
impl TransferEngine {
    pub const fn new()->Self {Self{tracker:TransferTracker::new(),submitted_trbs:0}}
    pub fn pending_count(&self)->usize {self.tracker.pending_count()}
    pub fn submitted_trbs(&self)->usize {self.submitted_trbs}
    pub fn submit_get_device_descriptor(&mut self,ring:&mut ProducerRing,
      slot_id:u8,endpoint_id:u8,data_buffer:u64,length:u16)->Result<u64,TransferError>{
        if slot_id==0{return Err(TransferError::InvalidSlot);}
        if endpoint_id==0||endpoint_id>31{return Err(TransferError::InvalidEndpoint);}
        let trbs=get_device_descriptor_control_trbs(data_buffer,length)
            .map_err(|_|TransferError::InvalidBuffer)?;
        let start=ring.enqueue_index();
        if start!=self.submitted_trbs||start.checked_add(3).map_or(true,|end|end>LINK_INDEX+1){
            return Err(TransferError::RingFull);
        }
        if self.tracker.len==self.tracker.pending.len(){return Err(TransferError::TrackerFull);}
        let base=ring.physical_base();
        let setup_pointer=base+(start*TRB_BYTES) as u64;
        let data_pointer=base+((start+1)*TRB_BYTES) as u64;
        let completion_pointer=base+((start+2)*TRB_BYTES) as u64;
        // Transfer Events may identify the Data Stage TRB on a Short Packet,
        // or another TRB in the TD; retain all three valid correlation pointers.
        self.tracker.reserve(PendingTransfer{setup_trb_pointer:setup_pointer,
            data_trb_pointer:data_pointer,status_trb_pointer:completion_pointer,
            slot_id,endpoint_id})?;
        for trb in trbs {
            if !ring.push(trb) {return Err(TransferError::RingFull);}
            self.submitted_trbs+=1;
        }
        Ok(completion_pointer)
    }
    pub fn poll_one(&mut self,events:&mut EventRing)->TransferPoll {
        let event=match events.next_event(){Some(v)=>v,None=>return TransferPoll::Empty};
        if event.event_type!=EventType::Transfer{return TransferPoll::Unrelated(event);}
        match self.tracker.complete(&event){
            Ok(result)=>TransferPoll::Completed(result),
            Err(error)=>TransferPoll::Invalid(error),
        }
    }
}
