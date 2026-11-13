//! PCI configuration-space discovery through legacy mechanism #1.
//! This is enumeration only; it does not enable bus mastering or touch BAR contents.
use crate::arch::x86::{inl, outl};

#[derive(Clone, Copy, Debug)]
pub struct PciDevice {
    pub bus: u8, pub device: u8, pub function: u8,
    pub vendor_id: u16, pub device_id: u16,
    pub class: u8, pub subclass: u8, pub prog_if: u8, pub header_type: u8,
}

pub unsafe fn read_config(bus: u8, device: u8, function: u8, offset: u8) -> u32 {
    let address = 0x8000_0000u32
        | ((bus as u32) << 16) | ((device as u32) << 11)
        | ((function as u32) << 8) | ((offset as u32) & 0xFC);
    outl(0xCF8, address);
    inl(0xCFC)
}

pub unsafe fn enumerate<F: FnMut(PciDevice)>(mut visit: F) {
    for bus in 0u16..=255 {
        for dev in 0u8..32 {
            let id0 = read_config(bus as u8, dev, 0, 0);
            if id0 as u16 == 0xFFFF { continue; }
            let header = read_config(bus as u8, dev, 0, 0x0C);
            let functions = if ((header >> 16) as u8 & 0x80) != 0 { 8 } else { 1 };
            for fun in 0..functions {
                let id = read_config(bus as u8, dev, fun, 0);
                if id as u16 == 0xFFFF { continue; }
                let class_reg = read_config(bus as u8, dev, fun, 0x08);
                let hdr = read_config(bus as u8, dev, fun, 0x0C);
                visit(PciDevice {
                    bus: bus as u8, device: dev, function: fun,
                    vendor_id: id as u16, device_id: (id >> 16) as u16,
                    prog_if: (class_reg >> 8) as u8,
                    subclass: (class_reg >> 16) as u8,
                    class: (class_reg >> 24) as u8,
                    header_type: (hdr >> 16) as u8,
                });
            }
        }
    }
}
