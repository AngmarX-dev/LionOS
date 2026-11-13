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
    pub address_64bit:bool,pub doorbell_offset:u32,pub runtime_offset:u32,
    pub usb_status:u32,
    pub legacy_support_found:bool,pub bios_owned:bool,pub os_owned:bool,
    pub ext_caps_valid:bool,
}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum InitStatus {
    NoController, InvalidBar, InvalidCapabilities, OwnershipTimeout,
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
        let hccparams1=match read(&region,0x10){Some(v)=>v,None=>continue};
        let dboff=match read(&region,0x14){Some(v)=>v&!3,None=>continue};
        let rtsoff=match read(&region,0x18){Some(v)=>v&!0x1F,None=>continue};
        let op_status=match read(&region,cap_length as usize+4){Some(v)=>v,None=>continue};
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
            address_64bit:hccparams1&1!=0,doorbell_offset:dboff,runtime_offset:rtsoff,
            usb_status:op_status,legacy_support_found:legacy_found,bios_owned,os_owned,ext_caps_valid:ext_valid,
        });
    }
    None
}
pub fn discover()->Option<XhciInfo>{
    let mut found=None;
    unsafe{pci::enumerate(|device|{if found.is_none(){found=inspect(device);});}
    found
}
fn wait_until<F:FnMut()->bool>(mut condition:F)->bool{
    for _ in 0..WAIT_LIMIT{if condition(){return true;}unsafe{crate::arch::x86::pause();}}
    false
}
/// Request ownership, stop and reset the controller. Never starts Run/Stop or DMA.
pub fn initialize()->InitReport{
    let info=match discover(){Some(v)=>v,None=>return InitReport{status:InitStatus::NoController,ownership_requested:false,bios_released:false,controller_halted:false,reset_completed:false,controller_ready:false}};
    if !info.ext_caps_valid{return InitReport{status:InitStatus::InvalidCapabilities,ownership_requested:false,bios_released:false,controller_halted:false,reset_completed:false,controller_ready:false};}
    let device=unsafe{
        let mut selected=None;
        pci::enumerate(|d|{if d.bus==info.bus&&d.device==info.device&&d.function==info.function{selected=Some(d);}});
        match selected{Some(d)=>d,None=>return InitReport{status:InitStatus::NoController,ownership_requested:false,bios_released:false,controller_halted:false,reset_completed:false,controller_ready:false}}
    };
    let size=unsafe{pci::probe_memory_bar_size(&device,info.bar_index as usize)};
    if size.map_or(true,|s|s!=info.bar_size){return InitReport{status:InitStatus::InvalidBar,ownership_requested:false,bios_released:false,controller_halted:false,reset_completed:false,controller_ready:false};}
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
        if header&(1<<16)!=0{
            if !region.write32(offset,header|(1<<24)){return InitReport{status:InitStatus::InvalidCapabilities,ownership_requested:false,bios_released:false,controller_halted:false,reset_completed:false,controller_ready:false};}
            requested=true;
            bios_released=wait_until(||read(&region,offset).map_or(false,|v|v&(1<<16)==0));
            if !bios_released{return InitReport{status:InitStatus::OwnershipTimeout,ownership_requested:requested,bios_released:false,controller_halted:false,reset_completed:false,controller_ready:false};}
        }
    }
    let op=info.cap_length as usize;
    let command=match read(&region,op){Some(v)=>v,None=>return InitReport{status:InitStatus::InvalidCapabilities,ownership_requested:requested,bios_released,bios_released:false,controller_halted:false,reset_completed:false,controller_ready:false}};
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
