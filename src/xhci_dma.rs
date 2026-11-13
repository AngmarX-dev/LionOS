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
