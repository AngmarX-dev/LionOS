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
#[inline] pub unsafe fn read_config(bus:u8,dev:u8,fun:u8,off:u8)->u32{
 let a=0x8000_0000|((bus as u32)<<16)|((dev as u32)<<11)|((fun as u32)<<8)|((off as u32)&0xFC);
 outl(0xCF8,a);inl(0xCFC)
}
unsafe fn write_config(bus:u8,dev:u8,fun:u8,off:u8,value:u32){
 let a=0x8000_0000|((bus as u32)<<16)|((dev as u32)<<11)|((fun as u32)<<8)|((off as u32)&0xFC);
 outl(0xCF8,a);outl(0xCFC,value);
}
/// Probe a memory BAR's implemented size and restore every modified register.
/// Decoding and bus mastering are disabled during the reversible sizing probe.
pub unsafe fn probe_memory_bar_size(device:&PciDevice,index:usize)->Option<u64>{
 if index>=PCI_BAR_COUNT{return None;}
 let bar=device.bars[index];
 if bar.kind!=2&&bar.kind!=4{return None;}
 if bar.kind==4&&index+1>=PCI_BAR_COUNT{return None;}
 let off=0x10+(index as u8)*4;
 let command=read_config(device.bus,device.device,device.function,0x04) as u16;
 outl(0xCF8,0x8000_0000|((device.bus as u32)<<16)|((device.device as u32)<<11)|((device.function as u32)<<8)|0x04);
 outw(0xCFC,command&!0x0007);
 let low_original=read_config(device.bus,device.device,device.function,off);
 let high_original=if bar.kind==4{read_config(device.bus,device.device,device.function,off+4)}else{0};
 write_config(device.bus,device.device,device.function,off,u32::MAX);
 if bar.kind==4{write_config(device.bus,device.device,device.function,off+4,u32::MAX);}
 let low_mask=read_config(device.bus,device.device,device.function,off);
 let high_mask=if bar.kind==4{read_config(device.bus,device.device,device.function,off+4)}else{0};
 if bar.kind==4{write_config(device.bus,device.device,device.function,off+4,high_original);}
 write_config(device.bus,device.device,device.function,off,low_original);
 outl(0xCF8,0x8000_0000|((device.bus as u32)<<16)|((device.device as u32)<<11)|((device.function as u32)<<8)|0x04);
 outw(0xCFC,command);
 let mask=if bar.kind==4{((high_mask as u64)<<32)|((low_mask&0xFFFF_FFF0) as u64)}else{(low_mask&0xFFFF_FFF0) as u64};
 if mask==0{return None;}
 let size=if bar.kind==4{(!mask).wrapping_add(1)}else{((!(mask as u32)).wrapping_add(1)) as u64};
 if size==0||!size.is_power_of_two(){return None;}
 Some(size)
}
/// Disable PCI bus mastering without touching the adjacent W1C Status register.
pub unsafe fn disable_bus_master(device:&PciDevice)->bool{
 let address=0x8000_0000|((device.bus as u32)<<16)|((device.device as u32)<<11)|((device.function as u32)<<8)|0x04;
 let command=read_config(device.bus,device.device,device.function,0x04) as u16;
 outl(0xCF8,address);outw(0xCFC,command&!0x0004);
 (read_config(device.bus,device.device,device.function,0x04) as u16)&0x0004==0
}
/// Write only the 16-bit PCI Command register; avoid adjacent W1C Status bits.
pub unsafe fn enable_memory_space(device:&PciDevice)->bool{
 let address=0x8000_0000|((device.bus as u32)<<16)|((device.device as u32)<<11)|((device.function as u32)<<8)|0x04;
 let command=read_config(device.bus,device.device,device.function,0x04) as u16;
 outl(0xCF8,address);outw(0xCFC,command|0x0002);
 (read_config(device.bus,device.device,device.function,0x04) as u16)&0x0002!=0
}
unsafe fn decode_bars(bus:u8,dev:u8,fun:u8,header:u8)->([PciBar;PCI_BAR_COUNT],u8){
 let mut bars=[PciBar::EMPTY;PCI_BAR_COUNT];let mut count=0;
 if header&0x7F!=0{return(bars,count)}
 let mut i=0usize;
 while i<PCI_BAR_COUNT{
  let raw=read_config(bus,dev,fun,0x10+(i as u8)*4);
  if raw==0||raw==u32::MAX{i+=1;continue}
  if raw&1!=0{let base=(raw&0xFFFF_FFFC) as u64;if base!=0{bars[i]=PciBar{kind:1,base,prefetchable:false};count+=1}i+=1;continue}
  let mt=(raw>>1)&3;let pf=raw&8!=0;
  match mt{
   0|1=>{let base=(raw&0xFFFF_FFF0) as u64;if base!=0{bars[i]=PciBar{kind:if mt==0{2}else{3},base,prefetchable:pf};count+=1}i+=1}
   2 if i+1<PCI_BAR_COUNT=>{let hi=read_config(bus,dev,fun,0x10+((i+1) as u8)*4);let base=((hi as u64)<<32)|((raw&0xFFFF_FFF0) as u64);if base!=0{bars[i]=PciBar{kind:4,base,prefetchable:pf};count+=1}i+=2}
   _=>i+=1
  }
 }(bars,count)
}
pub unsafe fn enumerate<F:FnMut(PciDevice)>(mut visit:F){
 for bus in 0u16..=255{for dev in 0u8..32{
  let id0=read_config(bus as u8,dev,0,0);if id0 as u16==0xFFFF{continue}
  let h0=read_config(bus as u8,dev,0,0x0C);let funcs=if ((h0>>16) as u8&0x80)!=0{8}else{1};
  for fun in 0..funcs{
   let id=if fun==0{id0}else{read_config(bus as u8,dev,fun,0)};if id as u16==0xFFFF{continue}
   let cs=read_config(bus as u8,dev,fun,4);let cr=read_config(bus as u8,dev,fun,8);let hr=read_config(bus as u8,dev,fun,0x0C);let header=(hr>>16) as u8;
   let (bars,bar_count)=decode_bars(bus as u8,dev,fun,header);let irq=read_config(bus as u8,dev,fun,0x3C);
   visit(PciDevice{bus:bus as u8,device:dev,function:fun,vendor_id:id as u16,device_id:(id>>16) as u16,
    command:cs as u16,status:(cs>>16) as u16,revision_id:cr as u8,prog_if:(cr>>8) as u8,
    subclass:(cr>>16) as u8,class:(cr>>24) as u8,header_type:header,interrupt_line:irq as u8,
    interrupt_pin:(irq>>8) as u8,bars,bar_count});
  }
 }}
}
