//! Fixed-capacity PCI device registry and driver matching framework.
use crate::pci::{self,PciDevice};
pub const MAX_DEVICES:usize=256;pub const MAX_DRIVERS:usize=16;pub const NO_DRIVER:u8=0xFF;
pub const WILDCARD_U8:u8=0xFF;pub const WILDCARD_U16:u16=0xFFFF;
#[derive(Clone,Copy)] pub struct DriverMatch{pub class:u8,pub subclass:u8,pub prog_if:u8,pub vendor_id:u16,pub device_id:u16}
#[derive(Clone,Copy)] pub struct DeviceDriver{pub name:&'static str,pub matcher:DriverMatch,pub probe:fn(&PciDevice)->bool}
#[derive(Clone,Copy)] pub struct DeviceRecord{pub pci:PciDevice,pub driver_index:u8}
static mut DEVICES:[DeviceRecord;MAX_DEVICES]=[DeviceRecord{pci:PciDevice::EMPTY,driver_index:NO_DRIVER};MAX_DEVICES];
static mut DRIVERS:[Option<DeviceDriver>;MAX_DRIVERS]=[None;MAX_DRIVERS];
static mut DEVICE_COUNT:usize=0;static mut DRIVER_COUNT:usize=0;static mut BOUND_COUNT:usize=0;static mut DROPPED_COUNT:usize=0;
fn matches(m:DriverMatch,d:&PciDevice)->bool{
 (m.class==WILDCARD_U8||m.class==d.class)&&(m.subclass==WILDCARD_U8||m.subclass==d.subclass)&&
 (m.prog_if==WILDCARD_U8||m.prog_if==d.prog_if)&&(m.vendor_id==WILDCARD_U16||m.vendor_id==d.vendor_id)&&
 (m.device_id==WILDCARD_U16||m.device_id==d.device_id)
}
/// Register descriptors before discovery. Registration does not touch hardware.
pub unsafe fn register_driver(driver:DeviceDriver)->Result<u8,()>{
 if DRIVER_COUNT>=MAX_DRIVERS{return Err(())}let i=DRIVER_COUNT;DRIVERS[i]=Some(driver);DRIVER_COUNT+=1;Ok(i as u8)
}
/// Run passive discovery. Probe callbacks are supplied by future concrete drivers.
pub unsafe fn discover(){
 DEVICE_COUNT=0;BOUND_COUNT=0;DROPPED_COUNT=0;
 pci::enumerate(|d|{
  crate::trace_pci_step(10);
  crate::trace_pci_step(15);
  if DEVICE_COUNT>=MAX_DEVICES{crate::trace_pci_step(16);DROPPED_COUNT=DROPPED_COUNT.saturating_add(1);return}
  let mut bound=NO_DRIVER;
  for i in 0..DRIVER_COUNT{if let Some(driver)=DRIVERS[i]{if matches(driver.matcher,&d)&&(driver.probe)(&d){bound=i as u8;BOUND_COUNT+=1;break}}}
  crate::trace_pci_step(11);
  let slot=DEVICE_COUNT;
  crate::trace_pci_step(14);
  core::ptr::write_volatile(core::ptr::addr_of_mut!(DEVICES[slot]),DeviceRecord{pci:d,driver_index:bound});
  crate::trace_pci_step(12);
  DEVICE_COUNT=slot+1;
  crate::trace_pci_step(13);
 });
}
pub fn device_count()->usize{unsafe{DEVICE_COUNT}}pub fn driver_count()->usize{unsafe{DRIVER_COUNT}}
pub fn bound_count()->usize{unsafe{BOUND_COUNT}}pub fn dropped_count()->usize{unsafe{DROPPED_COUNT}}
pub fn get(i:usize)->Option<DeviceRecord>{if i>=device_count(){return None}unsafe{Some(core::ptr::read_volatile(core::ptr::addr_of!(DEVICES[i])))}}
pub fn driver_name(i:usize)->Option<&'static str>{if i>=MAX_DRIVERS{return None}unsafe{DRIVERS[i].map(|d|d.name)}}
