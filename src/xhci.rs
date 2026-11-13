//! xHCI discovery, BIOS ownership handoff and bounded controller reset.
use crate::mmio::MmioRegion;
use crate::pci::{self,PciDevice};

const XHCI_CLASS:u8=0x0C;
const XHCI_SUBCLASS:u8=0x03;
const XHCI_PROGIF:u8=0x30;
const PROBE_WINDOW:usize=0x1000;
const WAIT_LIMIT:u32=5_000_000;
const USBCMD_RS:u32=1;
const USBCMD_HCRST:u32=1<<1;
const USBSTS_HCH:u32=1;
const USBSTS_CNR:u32=1<<11;

#[derive(Clone,Copy,Debug)]
pub struct XhciInfo{
    pub bus:u8,pub device:u8,pub function:u8,
    pub vendor_id:u16,pub device_id:u16,
    pub bar_index:u8,pub bar_base:u32,pub bar_size:u64,
    pub version:u16,pub cap_length:u8,
    pub max_slots:u8,pub max_interrupters:u16,pub max_ports:u8,
    pub scratchpad_count:u16,pub context_size:usize,pub page_4k_supported:bool,
    pub address_64bit:bool,pub doorbell_offset:u32,pub runtime_offset:u32,
    pub usb_status:u32,
    pub legacy_support_found:bool,pub bios_owned:bool,pub os_owned:bool,
    pub ext_caps_valid:bool,
}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum InitStatus {
    NoController, InvalidBar, InvalidCapabilities, UnsupportedPageSize, OwnershipTimeout,
    StopTimeout, ResetTimeout, NotReadyTimeout, ReadyStopped,
}
#[derive(Clone,Copy,Debug)]
pub struct InitReport {
    pub status:InitStatus,
    pub ownership_requested:bool,
    pub bios_released:bool,
    pub controller_halted:bool,
    pub reset_completed:bool,
    pub controller_ready:bool,
}
fn read(region:&MmioRegion,offset:usize)->Option<u32>{region.read32(offset)}
fn legacy_cap_offset(region:&MmioRegion,hccparams:u32)->Result<Option<usize>,()>{
    let mut offset=(((hccparams>>16)&0xFFFF) as usize).checked_mul(4).ok_or(())?;
    if offset==0{return Ok(None);}
    for _ in 0..64{
        if offset<0x20||offset.checked_add(4).map_or(true,|end|end>PROBE_WINDOW)||offset&3!=0{return Err(());}
        let header=read(region,offset).ok_or(())?;
        if header==0{return Ok(None);}
        let id=(header&0xFF) as u8;
        let next=((header>>8)&0xFF) as usize;
        if id==1{return Ok(Some(offset));}
        if next==0{return Ok(None);}
        offset=offset.checked_add(next.saturating_mul(4)).ok_or(())?;
    }
    Err(())
}
unsafe fn inspect(device:PciDevice)->Option<XhciInfo>{
    if device.class!=XHCI_CLASS||device.subclass!=XHCI_SUBCLASS||device.prog_if!=XHCI_PROGIF{return None;}
    for index in 0..pci::PCI_BAR_COUNT{
        let bar=device.bars[index];
        if (bar.kind!=2&&bar.kind!=4)||bar.base==0||bar.base>u32::MAX as u64||bar.base&0xFFF!=0{continue;}
        let bar_size=match pci::probe_memory_bar_size(&device,index){Some(v)=>v,None=>continue};
        if bar_size<PROBE_WINDOW||bar.base.checked_add(bar_size).map_or(true,|end|end>0x1_0000_0000){continue;}
        let region=match MmioRegion::from_pci_bar(&device,index,PROBE_WINDOW){Some(v)=>v,None=>continue};
        let cap0=match read(&region,0){Some(v)=>v,None=>continue};
        let cap_length=(cap0&0xFF) as u8;
        let version=(cap0>>16) as u16;
        if cap_length<0x20||cap_length as usize+8>PROBE_WINDOW||version==0||version==0xFFFF{continue;}
        let hcsparams1=match read(&region,0x04){Some(v)=>v,None=>continue};
        let hcsparams2=match read(&region,0x08){Some(v)=>v,None=>continue};
        let hccparams1=match read(&region,0x10){Some(v)=>v,None=>continue};
        let dboff=match read(&region,0x14){Some(v)=>v&!3,None=>continue};
        let rtsoff=match read(&region,0x18){Some(v)=>v&!0x1F,None=>continue};
        let op_status=match read(&region,cap_length as usize+4){Some(v)=>v,None=>continue};
        let page_size=match read(&region,cap_length as usize+8){Some(v)=>v,None=>continue};
        let (legacy_found,bios_owned,os_owned,ext_valid)=match legacy_cap_offset(&region,hccparams1){
            Ok(Some(offset))=>{let h=read(&region,offset)?;(true,h&(1<<16)!=0,h&(1<<24)!=0,true)},
            Ok(None)=>(false,false,false,true),
            Err(_)=>(false,false,false,false),
        };
        return Some(XhciInfo{
            bus:device.bus,device:device.device,function:device.function,
            vendor_id:device.vendor_id,device_id:device.device_id,
            bar_index:index as u8,bar_base:bar.base as u32,bar_size,
            version,cap_length,max_slots:(hcsparams1&0xFF) as u8,
            max_interrupters:((hcsparams1>>8)&0x7FF) as u16,max_ports:(hcsparams1>>24) as u8,
            scratchpad_count:((((hcsparams2>>21)&0x1F)<<5)|((hcsparams2>>27)&0x1F)) as u16,
            context_size:if hccparams1&(1<<2)!=0{64}else{32},page_4k_supported:page_size&1!=0,
            address_64bit:hccparams1&1!=0,doorbell_offset:dboff,runtime_offset:rtsoff,
            usb_status:op_status,legacy_support_found:legacy_found,bios_owned,os_owned,ext_caps_valid:ext_valid,
        });
    }
    None
}

/// Decoded USBSTS health snapshot. USBSTS is read-only/W1C-sensitive; this
/// decoder performs no register writes and intentionally preserves raw bits.
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub struct ControllerHealth {
    pub halted:bool,
    pub host_system_error:bool,
    pub event_interrupt:bool,
    pub port_change_detect:bool,
    pub save_restore_error:bool,
    pub controller_not_ready:bool,
    pub host_controller_error:bool,
    pub raw:u32,
}
impl ControllerHealth {
    pub const fn decode(raw:u32)->Self {
        Self {
            halted:raw&(1<<0)!=0,
            host_system_error:raw&(1<<2)!=0,
            event_interrupt:raw&(1<<3)!=0,
            port_change_detect:raw&(1<<4)!=0,
            save_restore_error:raw&(1<<10)!=0,
            controller_not_ready:raw&(1<<11)!=0,
            host_controller_error:raw&(1<<12)!=0,
            raw,
        }
    }
    pub const fn has_fatal_error(self)->bool {
        self.host_system_error||self.host_controller_error||self.save_restore_error
    }
}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum RecoveryAction {
    None,
    WaitUntilReady,
    StopAndInspect,
    QuarantineDmaAndRequireReset,
}
/// Conservative decision only: this function does not touch MMIO or DMA.
pub const fn recovery_action(health:ControllerHealth)->RecoveryAction {
    if health.has_fatal_error() {
        RecoveryAction::QuarantineDmaAndRequireReset
    } else if health.controller_not_ready {
        RecoveryAction::WaitUntilReady
    } else if health.halted {
        RecoveryAction::StopAndInspect
    } else {
        RecoveryAction::None
    }
}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum AbortPreflight {
    Eligible,
    ControllerHalted,
    ControllerNotReady,
    CommandRingNotRunning,
    BusMasterDisabled,
    FatalControllerError,
}
/// Validate the prerequisites for a future Command Abort implementation.
/// The caller supplies a fresh USBSTS/CRCR snapshot and PCI bus-master state.
pub const fn command_abort_preflight(health:ControllerHealth,crcr_low:u32,bus_master_enabled:bool)->AbortPreflight {
    if health.has_fatal_error(){AbortPreflight::FatalControllerError}
    else if health.controller_not_ready{AbortPreflight::ControllerNotReady}
    else if health.halted{AbortPreflight::ControllerHalted}
    else if !bus_master_enabled{AbortPreflight::BusMasterDisabled}
    else if crcr_low&(1<<3)==0{AbortPreflight::CommandRingNotRunning}
    else{AbortPreflight::Eligible}
}

pub fn discover()->Option<XhciInfo>{
    let mut found=None;
    unsafe { pci::enumerate(|device| { if found.is_none() { found=inspect(device); } }); }
    found
}
fn wait_until<F:FnMut()->bool>(mut condition:F)->bool{
    for _ in 0..WAIT_LIMIT{if condition(){return true;}unsafe{crate::arch::x86::pause();}}
    false
}
/// Request ownership, stop and reset the controller. Never starts Run/Stop or DMA.

#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum DmaProgramStatus{
    ProgrammedStopped,NoController,InvalidCapabilities,InvalidDmaLayout,
    BusMasterDisableFailed,ControllerNotHalted,CommandRingBusy,
    MmioUnavailable,RegisterWriteFailed,
}
fn write64(region:&MmioRegion,offset:usize,value:u64)->bool{
    // On this 32-bit kernel, write the high dword before committing the low dword.
    region.write32(offset+4,(value>>32) as u32)&&region.write32(offset,value as u32)
}
/// Install DMA base/ring pointers while xHCI is halted. Bus mastering and Run/Stop
/// remain disabled; this does not issue commands or enable interrupter delivery.
pub fn program_dma_registers(info:&XhciInfo,dma:&crate::xhci_dma::XhciDma)->DmaProgramStatus{
    if !info.page_4k_supported||info.max_slots==0||info.max_interrupters==0
        ||info.context_size!=32&&info.context_size!=64{
        return DmaProgramStatus::InvalidCapabilities;
    }
    let dcbaa=dma.dcbaa_base();
    let command_ring=dma.command_ring_base();
    let erst=dma.event_ring.erst_base();
    let event=dma.event_ring.segment_base();
    if dcbaa==0||dcbaa&0x3F!=0||command_ring==0||command_ring&0x3F!=0
        ||erst==0||erst&0x3F!=0||event==0||event&0xF!=0
        ||[dcbaa,command_ring,erst,event].iter().any(|v|*v>=0x1_0000_0000)
        ||dma.event_ring.erst_size()!=1||dma.event_ring.segment_trb_count()==0
        ||dma.dcbaa_entries()<=1{
        return DmaProgramStatus::InvalidDmaLayout;
    }
    let device=unsafe{
        let mut selected=None;
        pci::enumerate(|d|{if d.bus==info.bus&&d.device==info.device&&d.function==info.function
            &&d.vendor_id==info.vendor_id&&d.device_id==info.device_id{selected=Some(d);}});
        match selected{Some(d)=>d,None=>return DmaProgramStatus::NoController}
    };
    let current_size=unsafe{pci::probe_memory_bar_size(&device,info.bar_index as usize)};
    if current_size.map_or(true,|s|s!=info.bar_size){return DmaProgramStatus::InvalidCapabilities;}
    if !unsafe{pci::disable_bus_master(&device)}{return DmaProgramStatus::BusMasterDisableFailed;}
    let op=info.cap_length as usize;
    let runtime=info.runtime_offset as usize;
    let interrupter=match runtime.checked_add(0x20){Some(v)=>v,None=>return DmaProgramStatus::InvalidCapabilities};
    let op_end=match op.checked_add(0x38){Some(v)=>v,None=>return DmaProgramStatus::InvalidCapabilities};
    let runtime_end=match runtime.checked_add(0x40){Some(v)=>v,None=>return DmaProgramStatus::InvalidCapabilities};
    let required=core::cmp::max(PROBE_WINDOW,core::cmp::max(op_end,runtime_end));
    if runtime<0x20||runtime&0x1F!=0||required as u64>info.bar_size
        ||required>0x10_0000{
        return DmaProgramStatus::InvalidCapabilities;
    }
    let region=match unsafe{MmioRegion::from_pci_bar(&device,info.bar_index as usize,required)}{
        Some(v)=>v,None=>return DmaProgramStatus::MmioUnavailable
    };
    let command=match read(&region,op){Some(v)=>v,None=>return DmaProgramStatus::MmioUnavailable};
    let status=match read(&region,op+4){Some(v)=>v,None=>return DmaProgramStatus::MmioUnavailable};
    if command&USBCMD_RS!=0||status&USBSTS_HCH==0||status&USBSTS_CNR!=0{
        return DmaProgramStatus::ControllerNotHalted;
    }
    let crcr_low=match read(&region,op+0x18){Some(v)=>v,None=>return DmaProgramStatus::MmioUnavailable};
    if crcr_low&(1<<3)!=0{return DmaProgramStatus::CommandRingBusy;}
    // Interrupter 0 is configured but left disabled. IMAN.IP is W1C, so write
    // zero rather than echoing the current value and accidentally acknowledging it.
    if !region.write32(interrupter,0)||!region.write32(interrupter+4,0){
        return DmaProgramStatus::RegisterWriteFailed;
    }
    if !write64(&region,op+0x30,dcbaa)
        ||!write64(&region,op+0x18,command_ring|1)
        ||!region.write32(interrupter+8,dma.event_ring.erst_size() as u32)
        ||!write64(&region,interrupter+0x10,erst)
        ||!write64(&region,interrupter+0x18,event){
        return DmaProgramStatus::RegisterWriteFailed;
    }
    // No CONFIG.MaxSlotsEn, USBCMD.INTE, PCI bus-master enable, or USBCMD.RS.
    DmaProgramStatus::ProgrammedStopped
}

pub fn initialize()->InitReport{
    let info=match discover(){Some(v)=>v,None=>return InitReport{status:InitStatus::NoController,ownership_requested:false,bios_released:false,controller_halted:false,reset_completed:false,controller_ready:false}};
    if !info.ext_caps_valid{return InitReport{status:InitStatus::InvalidCapabilities,ownership_requested:false,bios_released:false,controller_halted:false,reset_completed:false,controller_ready:false};}
    if !info.page_4k_supported{return InitReport{status:InitStatus::UnsupportedPageSize,ownership_requested:false,bios_released:false,controller_halted:false,reset_completed:false,controller_ready:false};}
    let device=unsafe{
        let mut selected=None;
        pci::enumerate(|d|{if d.bus==info.bus&&d.device==info.device&&d.function==info.function{selected=Some(d);}});
        match selected{Some(d)=>d,None=>return InitReport{status:InitStatus::NoController,ownership_requested:false,bios_released:false,controller_halted:false,reset_completed:false,controller_ready:false}}
    };
    let size=unsafe{pci::probe_memory_bar_size(&device,info.bar_index as usize)};
    if size.map_or(true,|s|s!=info.bar_size){return InitReport{status:InitStatus::InvalidBar,ownership_requested:false,bios_released:false,controller_halted:false,reset_completed:false,controller_ready:false};}
    if !unsafe{pci::disable_bus_master(&device)}{
        return InitReport{status:InitStatus::InvalidBar,ownership_requested:false,bios_released:false,controller_halted:false,reset_completed:false,controller_ready:false};
    }
    let region=match unsafe{MmioRegion::from_pci_bar(&device,info.bar_index as usize,PROBE_WINDOW)}{
        Some(v)=>v,None=>return InitReport{status:InitStatus::InvalidBar,ownership_requested:false,bios_released:false,controller_halted:false,reset_completed:false,controller_ready:false}
    };
    let hcc=match read(&region,0x10){Some(v)=>v,None=>return InitReport{status:InitStatus::InvalidCapabilities,ownership_requested:false,bios_released:false,controller_halted:false,reset_completed:false,controller_ready:false}};
    let legacy=match legacy_cap_offset(&region,hcc){
        Ok(v)=>v,Err(_)=>return InitReport{status:InitStatus::InvalidCapabilities,ownership_requested:false,bios_released:false,controller_halted:false,reset_completed:false,controller_ready:false}
    };
    let mut requested=false;
    let mut bios_released=true;
    if let Some(offset)=legacy{
        let header=match read(&region,offset){Some(v)=>v,None=>return InitReport{status:InitStatus::InvalidCapabilities,ownership_requested:false,bios_released:false,controller_halted:false,reset_completed:false,controller_ready:false}};
        let bios_was_owner=header&(1<<16)!=0;
        if !region.write32(offset,header|(1<<24)){return InitReport{status:InitStatus::InvalidCapabilities,ownership_requested:false,bios_released:false,controller_halted:false,reset_completed:false,controller_ready:false};}
        requested=true;
        if bios_was_owner{
            bios_released=wait_until(||read(&region,offset).map_or(false,|v|v&(1<<16)==0));
            if !bios_released{return InitReport{status:InitStatus::OwnershipTimeout,ownership_requested:requested,bios_released:false,controller_halted:false,reset_completed:false,controller_ready:false};}
        }
    }
    let op=info.cap_length as usize;
    let command=match read(&region,op){Some(v)=>v,None=>return InitReport{status:InitStatus::InvalidCapabilities,ownership_requested:requested,bios_released,controller_halted:false,reset_completed:false,controller_ready:false}};
    if !region.write32(op,command&!USBCMD_RS){return InitReport{status:InitStatus::InvalidCapabilities,ownership_requested:requested,bios_released,controller_halted:false,reset_completed:false,controller_ready:false};}
    let halted=wait_until(||read(&region,op+4).map_or(false,|v|v&USBSTS_HCH!=0));
    if !halted{return InitReport{status:InitStatus::StopTimeout,ownership_requested:requested,bios_released,controller_halted:false,reset_completed:false,controller_ready:false};}
    let stopped_cmd=match read(&region,op){Some(v)=>v,None=>return InitReport{status:InitStatus::InvalidCapabilities,ownership_requested:requested,bios_released,controller_halted:true,reset_completed:false,controller_ready:false}};
    if !region.write32(op,stopped_cmd|USBCMD_HCRST){return InitReport{status:InitStatus::InvalidCapabilities,ownership_requested:requested,bios_released,controller_halted:true,reset_completed:false,controller_ready:false};}
    let reset=wait_until(||read(&region,op).map_or(false,|v|v&USBCMD_HCRST==0));
    if !reset{return InitReport{status:InitStatus::ResetTimeout,ownership_requested:requested,bios_released,controller_halted:true,reset_completed:false,controller_ready:false};}
    let ready=wait_until(||read(&region,op+4).map_or(false,|v|v&USBSTS_CNR==0));
    if !ready{return InitReport{status:InitStatus::NotReadyTimeout,ownership_requested:requested,bios_released,controller_halted:true,reset_completed:true,controller_ready:false};}
    InitReport{status:InitStatus::ReadyStopped,ownership_requested:requested,bios_released,controller_halted:true,reset_completed:true,controller_ready:true}
}


/// Read-only snapshot of one xHCI root-port PORTSC register.
/// Change bits are reported, never acknowledged here (PORTSC is W1C-sensitive).
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub struct PortStatus {
    pub connected:bool,pub enabled:bool,pub over_current:bool,
    pub reset:bool,pub link_state:u8,pub powered:bool,pub speed:u8,
    pub connect_change:bool,pub enable_change:bool,pub warm_reset_change:bool,
    pub over_current_change:bool,pub reset_change:bool,pub link_change:bool,
    pub config_error_change:bool,pub raw:u32,
}
impl PortStatus {
    pub const fn decode(raw:u32)->Self {
        Self {
            connected:raw&(1<<0)!=0,enabled:raw&(1<<1)!=0,
            over_current:raw&(1<<3)!=0,reset:raw&(1<<4)!=0,
            link_state:((raw>>5)&0xF) as u8,powered:raw&(1<<9)!=0,
            speed:((raw>>10)&0xF) as u8,
            connect_change:raw&(1<<17)!=0,enable_change:raw&(1<<18)!=0,
            warm_reset_change:raw&(1<<19)!=0,over_current_change:raw&(1<<20)!=0,
            reset_change:raw&(1<<21)!=0,link_change:raw&(1<<22)!=0,
            config_error_change:raw&(1<<23)!=0,raw,
        }
    }
    pub const fn has_change(self)->bool {
        self.connect_change||self.enable_change||self.warm_reset_change||
        self.over_current_change||self.reset_change||self.link_change||
        self.config_error_change
    }
}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum PortReadError { InvalidPort,InvalidBar,OutOfRange,DeviceNotFound,ReadFailed }
/// Read PORTSC for a 1-based root-port number. This is strictly read-only.
pub fn read_port_status(info:&XhciInfo,port:u8)->Result<PortStatus,PortReadError>{
    if port==0||port>info.max_ports{return Err(PortReadError::InvalidPort);}
    let device=unsafe{
        let mut selected=None;
        pci::enumerate(|d|{if d.bus==info.bus&&d.device==info.device&&d.function==info.function{selected=Some(d);}});
        selected.ok_or(PortReadError::DeviceNotFound)?
    };
    let offset=(info.cap_length as usize).checked_add(0x400)
        .and_then(|v|v.checked_add((port as usize-1)*0x10))
        .ok_or(PortReadError::OutOfRange)?;
    let end=offset.checked_add(4).ok_or(PortReadError::OutOfRange)?;
    if end as u64>info.bar_size{return Err(PortReadError::OutOfRange);}
    let region=unsafe{MmioRegion::from_pci_bar(&device,info.bar_index as usize,end)}
        .ok_or(PortReadError::InvalidBar)?;
    let raw=region.read32(offset).ok_or(PortReadError::ReadFailed)?;
    Ok(PortStatus::decode(raw))
}
