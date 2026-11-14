//! PCI discovery, passive BAR decoding, reversible BAR sizing and command control.
use crate::arch::x86::{inl,outl,outw};
pub const PCI_BAR_COUNT:usize=6;
#[derive(Clone,Copy,Debug)]
pub struct PciBar{pub kind:u8,pub base:u64,pub prefetchable:bool}
impl PciBar{pub const EMPTY:Self=Self{kind:0,base:0,prefetchable:false};}
#[derive(Clone,Copy,Debug)]
pub struct PciDevice{
 pub bus:u8,pub device:u8,pub function:u8,pub vendor_id:u16,pub device_id:u16,
 pub command:u16,pub status:u16,pub revision_id:u8,pub prog_if:u8,pub subclass:u8,pub class:u8,
 pub header_type:u8,pub interrupt_line:u8,pub interrupt_pin:u8,pub bars:[PciBar;PCI_BAR_COUNT],pub bar_count:u8,
}
impl PciDevice {
 pub const EMPTY:Self=Self{bus:0,device:0,function:0,vendor_id:0,device_id:0,command:0,status:0,revision_id:0,prog_if:0,subclass:0,class:0,header_type:0,interrupt_line:0,interrupt_pin:0,bars:[PciBar::EMPTY;PCI_BAR_COUNT],bar_count:0};
}
#[inline] pub unsafe fn read_config(bus:u8,dev:u8,fun:u8,off:u8)->u32{
 let a=0x8000_0000|((bus as u32)<<16)|((dev as u32)<<11)|((fun as u32)<<8)|((off as u32)&0xFC);
 outl(0xCF8,a);inl(0xCFC)
}
unsafe fn write_config(bus:u8,dev:u8,fun:u8,off:u8,value:u32){
 let a=0x8000_0000|((bus as u32)<<16)|((dev as u32)<<11)|((fun as u32)<<8)|((off as u32)&0xFC);
 outl(0xCF8,a);outl(0xCFC,value);
}
pub unsafe fn probe_memory_bar_size(device:&PciDevice,index:usize)->Option<u64>{
 if index>=PCI_BAR_COUNT{return None;}let bar=device.bars[index];if bar.kind!=2&&bar.kind!=4{return None;}if bar.kind==4&&index+1>=PCI_BAR_COUNT{return None;}
 let off=0x10+(index as u8)*4;let command=read_config(device.bus,device.device,device.function,0x04) as u16;
 outl(0xCF8,0x8000_0000|((device.bus as u32)<<16)|((device.device as u32)<<11)|((device.function as u32)<<8)|0x04);outw(0xCFC,command&!0x0007);
 let low_original=read_config(device.bus,device.device,device.function,off);let high_original=if bar.kind==4{read_config(device.bus,device.device,device.function,off+4)}else{0};
 write_config(device.bus,device.device,device.function,off,u32::MAX);if bar.kind==4{write_config(device.bus,device.device,device.function,off+4,u32::MAX);}
 let low_mask=read_config(device.bus,device.device,device.function,off);let high_mask=if bar.kind==4{read_config(device.bus,device.device,device.function,off+4)}else{0};
 if bar.kind==4{write_config(device.bus,device.device,device.function,off+4,high_original);}write_config(device.bus,device.device,device.function,off,low_original);
 outl(0xCF8,0x8000_0000|((device.bus as u32)<<16)|((device.device as u32)<<11)|((device.function as u32)<<8)|0x04);outw(0xCFC,command);
 let mask=if bar.kind==4{((high_mask as u64)<<32)|((low_mask&0xFFFF_FFF0) as u64)}else{(low_mask&0xFFFF_FFF0) as u64};if mask==0{return None;}
 let size=if bar.kind==4{(!mask).wrapping_add(1)}else{((!(mask as u32)).wrapping_add(1)) as u64};if size==0||!size.is_power_of_two(){return None;}Some(size)
}
pub unsafe fn disable_bus_master(device:&PciDevice)->bool{let address=0x8000_0000|((device.bus as u32)<<16)|((device.device as u32)<<11)|((device.function as u32)<<8)|0x04;let command=read_config(device.bus,device.device,device.function,0x04) as u16;outl(0xCF8,address);outw(0xCFC,command&!0x0004);(read_config(device.bus,device.device,device.function,0x04) as u16)&0x0004==0}
pub unsafe fn enable_memory_space(device:&PciDevice)->bool{let address=0x8000_0000|((device.bus as u32)<<16)|((device.device as u32)<<11)|((device.function as u32)<<8)|0x04;let command=read_config(device.bus,device.device,device.function,0x04) as u16;outl(0xCF8,address);outw(0xCFC,command|0x0002);(read_config(device.bus,device.device,device.function,0x04) as u16)&0x0002!=0}
// Reused static descriptor keeps early enumeration off the limited boot stack.
static mut SCAN_DESCRIPTOR:PciDevice=PciDevice::EMPTY;
/// Early-boot enumeration intentionally defers all BAR probing.
pub unsafe fn enumerate<F:FnMut(&PciDevice)>(mut visit:F){
 for bus in 0u16..=255{for dev in 0u8..32{
  let id0=read_config(bus as u8,dev,0,0);if bus==0&&dev==0{crate::trace_pci_step(1);}if id0 as u16==0xFFFF{continue}
  let h0=read_config(bus as u8,dev,0,0x0C);let funcs=if((h0>>16) as u8&0x80)!=0{8}else{1};if bus==0&&dev==0{crate::trace_pci_step(4);}
  for fun in 0..funcs{
   let id=if fun==0{id0}else{read_config(bus as u8,dev,fun,0)};if id as u16==0xFFFF{continue}
   let cs=read_config(bus as u8,dev,fun,4);let cr=read_config(bus as u8,dev,fun,8);let hr=read_config(bus as u8,dev,fun,0x0C);let header=(hr>>16) as u8;
   if bus==0&&dev==0&&fun==0{crate::trace_pci_step(5);crate::trace_pci_step(6);crate::trace_pci_step(7);crate::trace_pci_step(8);}
   let irq=read_config(bus as u8,dev,fun,0x3C);
   if bus==0&&dev==0&&fun==0{crate::trace_pci_step(17);}
   let descriptor=core::ptr::addr_of_mut!(SCAN_DESCRIPTOR);
   core::ptr::addr_of_mut!((*descriptor).bus).write(bus as u8);
   core::ptr::addr_of_mut!((*descriptor).device).write(dev);
   core::ptr::addr_of_mut!((*descriptor).function).write(fun);
   core::ptr::addr_of_mut!((*descriptor).vendor_id).write(id as u16);
   core::ptr::addr_of_mut!((*descriptor).device_id).write((id>>16) as u16);
   core::ptr::addr_of_mut!((*descriptor).command).write(cs as u16);
   core::ptr::addr_of_mut!((*descriptor).status).write((cs>>16) as u16);
   core::ptr::addr_of_mut!((*descriptor).revision_id).write(cr as u8);
   core::ptr::addr_of_mut!((*descriptor).prog_if).write((cr>>8) as u8);
   core::ptr::addr_of_mut!((*descriptor).subclass).write((cr>>16) as u8);
   core::ptr::addr_of_mut!((*descriptor).class).write((cr>>24) as u8);
   core::ptr::addr_of_mut!((*descriptor).header_type).write(header);
   core::ptr::addr_of_mut!((*descriptor).interrupt_line).write(irq as u8);
   core::ptr::addr_of_mut!((*descriptor).interrupt_pin).write((irq>>8) as u8);
   if bus==0&&dev==0&&fun==0{crate::trace_pci_step(19);crate::trace_pci_step(18);crate::trace_pci_step(9);crate::trace_pci_step(2);}
   visit(&*descriptor);
   if bus==0&&dev==0&&fun==0{crate::trace_pci_step(3);}
  }
 }}
}
