#![no_std]
#![no_main]

use core::arch::asm;
use core::panic::PanicInfo;
use core::ptr::{read_unaligned, write_volatile};

const MULTIBOOT2_BOOT_MAGIC: u32 = 0x36D7_6289;
const VGA_TEXT: *mut u16 = 0xB8000 as *mut u16;
const VGA_COLUMNS: usize = 80;
const VGA_ROWS: usize = 25;
const MAX_BOOT_INFO: u32 = 16 * 1024 * 1024;

static mut CURSOR: usize = 0;
static mut COLOR: u8 = 0x0F;

#[inline]
unsafe fn outb(port: u16, value: u8) {
    asm!("out dx, al", in("dx") port, in("al") value, options(nomem, nostack, preserves_flags));
}

#[inline]
unsafe fn inl(port: u16) -> u32 {
    let value: u32;
    asm!("in eax, dx", in("dx") port, out("eax") value, options(nomem, nostack, preserves_flags));
    value
}

#[inline]
unsafe fn outl(port: u16, value: u32) {
    asm!("out dx, eax", in("dx") port, in("eax") value, options(nomem, nostack, preserves_flags));
}

fn serial_init() {
    unsafe {
        outb(0x3F9, 0x00);
        outb(0x3FB, 0x80);
        outb(0x3F8, 0x01);
        outb(0x3F9, 0x00);
        outb(0x3FB, 0x03);
        outb(0x3FA, 0xC7);
        outb(0x3FC, 0x0B);
    }
}

fn serial_write_byte(byte: u8) {
    unsafe {
        for _ in 0..100_000 {
            if inb(0x3FD) & 0x20 != 0 {
                outb(0x3F8, byte);
                return;
            }
            asm!("pause", options(nomem, nostack, preserves_flags));
        }
    }
}

#[inline]
unsafe fn inb(port: u16) -> u8 {
    let value: u8;
    asm!("in al, dx", in("dx") port, out("al") value, options(nomem, nostack, preserves_flags));
    value
}

fn put_byte(byte: u8) {
    serial_write_byte(byte);
    unsafe {
        if byte == b'\n' {
            CURSOR = (CURSOR / VGA_COLUMNS + 1) * VGA_COLUMNS;
        } else if byte == b'\r' {
            CURSOR -= CURSOR % VGA_COLUMNS;
        } else {
            if CURSOR < VGA_COLUMNS * VGA_ROWS {
                write_volatile(VGA_TEXT.add(CURSOR), ((COLOR as u16) << 8) | byte as u16);
            }
            CURSOR += 1;
        }

        if CURSOR >= VGA_COLUMNS * VGA_ROWS {
            for index in 0..(VGA_COLUMNS * (VGA_ROWS - 1)) {
                let cell = read_volatile_u16(VGA_TEXT.add(index + VGA_COLUMNS));
                write_volatile(VGA_TEXT.add(index), cell);
            }
            for index in (VGA_COLUMNS * (VGA_ROWS - 1))..(VGA_COLUMNS * VGA_ROWS) {
                write_volatile(VGA_TEXT.add(index), ((COLOR as u16) << 8) | b' ' as u16);
            }
            CURSOR = VGA_COLUMNS * (VGA_ROWS - 1);
        }
    }
}

unsafe fn read_volatile_u16(pointer: *const u16) -> u16 {
    core::ptr::read_volatile(pointer)
}

fn write_str(value: &str) {
    for byte in value.bytes() {
        put_byte(byte);
    }
}

fn write_hex(value: u32) {
    const DIGITS: &[u8; 16] = b"0123456789ABCDEF";
    write_str("0x");
    for shift in (0..8).rev() {
        put_byte(DIGITS[((value >> (shift * 4)) & 0xF) as usize]);
    }
}

fn write_dec(mut value: u32) {
    let mut digits = [0u8; 10];
    let mut used = 0usize;
    if value == 0 {
        put_byte(b'0');
        return;
    }
    while value != 0 && used < digits.len() {
        digits[used] = b'0' + (value % 10) as u8;
        value /= 10;
        used += 1;
    }
    while used != 0 {
        used -= 1;
        put_byte(digits[used]);
    }
}

fn read_u32(base: *const u8, offset: usize) -> u32 {
    unsafe { read_unaligned(base.add(offset).cast::<u32>()) }
}

fn read_u64(base: *const u8, offset: usize) -> u64 {
    unsafe { read_unaligned(base.add(offset).cast::<u64>()) }
}

fn parse_multiboot2(info_address: u32) {
    if info_address == 0 {
        write_str("[ERR] Multiboot2 information pointer is null\n");
        return;
    }

    let info = info_address as *const u8;
    let total_size = read_u32(info, 0);
    if total_size < 16 || total_size > MAX_BOOT_INFO {
        write_str("[ERR] Multiboot2 information size rejected: ");
        write_dec(total_size);
        write_str("\n");
        return;
    }

    write_str("[ OK ] Multiboot2 information size: ");
    write_dec(total_size);
    write_str(" bytes\n");

    let mut offset = 8usize;
    let limit = total_size as usize;
    let mut usable_bytes = 0u64;
    let mut memory_map_seen = false;
    let mut framebuffer_seen = false;

    while offset + 8 <= limit {
        let tag = unsafe { info.add(offset) };
        let tag_type = read_u32(tag, 0);
        let tag_size = read_u32(tag, 4) as usize;

        if tag_size < 8 || offset.checked_add(tag_size).map_or(true, |end| end > limit) {
            write_str("[WARN] Invalid Multiboot2 tag; parsing stopped\n");
            break;
        }

        match tag_type {
            0 => break,
            6 if tag_size >= 16 => {
                let entry_size = read_u32(tag, 8) as usize;
                if entry_size >= 24 {
                    let mut entry_offset = 16usize;
                    while entry_offset + entry_size <= tag_size {
                        let entry = unsafe { tag.add(entry_offset) };
                        let base = read_u64(entry, 0);
                        let length = read_u64(entry, 8);
                        let kind = read_u32(entry, 16);
                        if kind == 1 {
                            usable_bytes = usable_bytes.saturating_add(length);
                            let _region_start = base;
                        }
                        entry_offset += entry_size;
                    }
                    memory_map_seen = true;
                }
            }
            8 if tag_size >= 32 => {
                let address = read_u64(tag, 8);
                let pitch = read_u32(tag, 16);
                let width = read_u32(tag, 20);
                let height = read_u32(tag, 24);
                let bpp = unsafe { *tag.add(28) };
                write_str("[ OK ] Firmware framebuffer: ");
                write_dec(width);
                put_byte(b'x');
                write_dec(height);
                write_str(" / ");
                write_dec(bpp as u32);
                write_str(" bpp / pitch ");
                write_dec(pitch);
                write_str(" / address ");
                write_hex((address >> 32) as u32);
                write_hex(address as u32);
                write_str("\n");
                framebuffer_seen = true;
            }
            _ => {}
        }

        offset = (offset + tag_size + 7) & !7;
    }

    if memory_map_seen {
        write_str("[ OK ] Usable physical memory: ");
        write_hex((usable_bytes >> 32) as u32);
        write_hex(usable_bytes as u32);
        write_str(" bytes (");
        write_hex((usable_bytes >> 20) as u32);
        write_str(" MiB low word)\n");
    } else {
        write_str("[WARN] No valid Multiboot2 memory-map tag\n");
    }

    if framebuffer_seen {
        write_str("[ OK ] Firmware display handoff preserved; no mode change attempted\n");
    } else {
        write_str("[WARN] No Multiboot2 framebuffer tag; VGA text output remains active\n");
    }
}

fn pci_scan_display_devices() {
    let mut found = 0u32;
    write_str("\nPCI display-class scan (bus 0):\n");

    for slot in 0u32..32 {
        let address = 0x8000_0000u32 | (slot << 11) | 0x08;
        unsafe {
            outl(0xCF8, address);
        }
        let class_register = unsafe { inl(0xCFC) };
        let class_code = (class_register >> 24) as u8;
        if class_code != 0x03 {
            continue;
        }

        let id_address = 0x8000_0000u32 | (slot << 11);
        unsafe {
            outl(0xCF8, id_address);
        }
        let id = unsafe { inl(0xCFC) };
        if id as u16 == 0xFFFF {
            continue;
        }

        write_str("  VGA device at 00:");
        write_hex(slot);
        write_str(" vendor=");
        write_hex(id as u16 as u32);
        write_str(" device=");
        write_hex((id >> 16) as u16 as u32);
        write_str("\n");
        found += 1;
    }

    write_str("Display functions found: ");
    write_dec(found);
    write_str("\n");
}

#[no_mangle]
pub extern "C" fn rust_kernel_main(magic: u32, multiboot_info: u32) -> ! {
    unsafe {
        COLOR = 0x0F;
        CURSOR = 0;
        for index in 0..(VGA_COLUMNS * VGA_ROWS) {
            write_volatile(VGA_TEXT.add(index), 0x0F20);
        }
    }

    serial_init();
    write_str("\nLionOS Rust Kernel - experimental i686 build\n");
    write_str("============================================\n");

    if magic != MULTIBOOT2_BOOT_MAGIC {
        write_str("[FATAL] Invalid Multiboot2 boot magic: ");
        write_hex(magic);
        write_str("\n");
        halt_forever();
    }

    write_str("[ OK ] Rust kernel entry / Multiboot2 magic verified\n");
    parse_multiboot2(multiboot_info);
    pci_scan_display_devices();

    write_str("\nRUST PORT STATUS\n");
    write_str("[ OK ] VGA text console\n");
    write_str("[ OK ] Serial diagnostics (COM1)\n");
    write_str("[ OK ] Multiboot2 memory-map and framebuffer discovery\n");
    write_str("[ OK ] Read-only PCI display-device discovery\n");
    write_str("[ -- ] Paging, allocator, scheduler, storage, networking and native GPU driver are not ported yet\n");
    write_str("\nLionOS Rust bootstrap is alive.\n");

    halt_forever()
}

fn halt_forever() -> ! {
    loop {
        unsafe {
            asm!("cli; hlt", options(nomem, nostack));
        }
    }
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    write_str("\n[FATAL] Rust kernel panic\n");
    halt_forever()
}

/*
 * Freestanding compiler support. Volatile accesses prevent LLVM from
 * lowering these implementations back into recursive libc calls.
 */
#[no_mangle]
pub unsafe extern "C" fn memset(destination: *mut u8, value: i32, count: usize) -> *mut u8 {
    for index in 0..count {
        write_volatile(destination.add(index), value as u8);
    }
    destination
}

#[no_mangle]
pub unsafe extern "C" fn memcpy(destination: *mut u8, source: *const u8, count: usize) -> *mut u8 {
    for index in 0..count {
        let byte = core::ptr::read_volatile(source.add(index));
        write_volatile(destination.add(index), byte);
    }
    destination
}

#[no_mangle]
pub unsafe extern "C" fn memmove(destination: *mut u8, source: *const u8, count: usize) -> *mut u8 {
    if (destination as usize) <= (source as usize) {
        for index in 0..count {
            let byte = core::ptr::read_volatile(source.add(index));
            write_volatile(destination.add(index), byte);
        }
    } else {
        for index in (0..count).rev() {
            let byte = core::ptr::read_volatile(source.add(index));
            write_volatile(destination.add(index), byte);
        }
    }
    destination
}

#[no_mangle]
pub unsafe extern "C" fn memcmp(left: *const u8, right: *const u8, count: usize) -> i32 {
    for index in 0..count {
        let a = core::ptr::read_volatile(left.add(index));
        let b = core::ptr::read_volatile(right.add(index));
        if a != b {
            return a as i32 - b as i32;
        }
    }
    0
}
