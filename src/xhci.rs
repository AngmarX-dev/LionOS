//! Passive xHCI capability discovery. This module does not claim controller
//! ownership, reset the controller, enable bus mastering, or start DMA.
use crate::mmio::MmioRegion;
use crate::pci::{self,PciDevice};

const XHCI_CLASS:u8=0x0C;
const XHCI_SUBCLASS:u8=0x03;
const XHCI_PROGIF:u8=0x30;
const PROBE_WINDOW:usize=0x1000;

#[derive(Clone,Copy,Debug)]
pub struct XhciInfo{
    pub bus:u8,pub device:u8,pub function:u8,
    pub vendor_id:u16,pub device_id:u16,
    pub bar_index:u8,pub bar_base:u32,
    pub version:u16,pub cap_length:u8,
    pub max_slots:u8,pub max_interrupters:u16,pub max_ports:u8,
    pub address_64bit:bool,pub doorbell_offset:u32,pub runtime_offset:u32,
    pub usb_status:u32,
    pub legacy_support_found:bool,pub bios_owned:bool,pub os_owned:bool,
    pub ext_caps_valid:bool,
}
fn read(region:&MmioRegion,offset:usize)->Option<u32>{region.read32(offset)}
fn legacy_ownership(region:&MmioRegion,hccparams:u32)->(bool,bool,bool,bool){
    let mut offset=(((hccparams>>16)&0xFFFF) as usize).checked_mul(4).unwrap_or(0);
    if offset==0{return(false,false,false,true);}
    let mut found=false;let mut bios=false;let mut os=false;
    for _ in 0..64{
        if offset<0x20||offset.checked_add(4).map_or(true,|end|end>PROBE_WINDOW)||offset&3!=0{
            return(found,bios,os,false);
        }
        let header=match read(region,offset){Some(v)=>v,None=>return(found,bios,os,false)};
        if header==0{break;}
        let id=(header&0xFF) as u8;
        let next=((header>>8)&0xFF) as usize;
        if id==1{
            found=true;
            bios=header&(1<<16)!=0;
            os=header&(1<<24)!=0;
            return(found,bios,os,true);
        }
        if next==0{break;}
        offset=match offset.checked_add(next.saturating_mul(4)){Some(v)=>v,None=>return(found,bios,os,false)};
    }
    (found,bios,os,true)
}
unsafe fn inspect(device:PciDevice)->Option<XhciInfo>{
    if device.class!=XHCI_CLASS||device.subclass!=XHCI_SUBCLASS||device.prog_if!=XHCI_PROGIF{return None;}
    for index in 0..pci::PCI_BAR_COUNT{
        let bar=device.bars[index];
        if (bar.kind!=2&&bar.kind!=4)||bar.base==0||bar.base>u32::MAX as u64||bar.base&0xFFF!=0{continue;}
        // xHCI register space is expected to expose at least the first 4 KiB.
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
        let (legacy_found,bios_owned,os_owned,ext_valid)=legacy_ownership(&region,hccparams1);
        return Some(XhciInfo{
            bus:device.bus,device:device.device,function:device.function,
            vendor_id:device.vendor_id,device_id:device.device_id,
            bar_index:index as u8,bar_base:bar.base as u32,
            version,cap_length,
            max_slots:(hcsparams1&0xFF) as u8,
            max_interrupters:((hcsparams1>>8)&0x7FF) as u16,
            max_ports:(hcsparams1>>24) as u8,
            address_64bit:hccparams1&1!=0,
            doorbell_offset:dboff,runtime_offset:rtsoff,usb_status:op_status,
            legacy_support_found:legacy_found,bios_owned,os_owned,ext_caps_valid:ext_valid,
        });
    }
    None
}

/// Find and inspect the first valid xHCI function. All reads are bounded to a
/// 4 KiB BAR window. The probe is passive: no controller register is written.
pub fn discover()->Option<XhciInfo>{
    let mut found=None;
    unsafe{
        pci::enumerate(|device|{
            if found.is_none(){found=inspect(device);}
        });
    }
    found
}
