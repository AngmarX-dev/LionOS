//! Conservative Multiboot2 framebuffer renderer. Supports packed RGB 24/32 bpp.
use core::ptr::write_volatile;

#[derive(Clone, Copy, Debug)]
pub struct Framebuffer {
    pub address: u32, pub pitch: u32, pub width: u32, pub height: u32, pub bpp: u8,
    pub red_pos: u8, pub red_size: u8, pub green_pos: u8, pub green_size: u8,
    pub blue_pos: u8, pub blue_size: u8,
}
static mut FB: Option<Framebuffer> = None;

pub unsafe fn configure(address: u64, pitch: u32, width: u32, height: u32, bpp: u8,
    red_pos: u8, red_size: u8, green_pos: u8, green_size: u8, blue_pos: u8, blue_size: u8) -> bool {
    if address == 0 || address > u32::MAX as u64 || width == 0 || height == 0
        || !matches!(bpp, 24 | 32)
        || red_size > 8 || green_size > 8 || blue_size > 8
        || red_pos as u16 + red_size as u16 > bpp as u16
        || green_pos as u16 + green_size as u16 > bpp as u16
        || blue_pos as u16 + blue_size as u16 > bpp as u16 { FB = None; return false; }
    let bytes = (bpp as u32 + 7) / 8;
    if pitch < width.saturating_mul(bytes) { FB = None; return false; }
    let end = (address as u32 as u64).saturating_add(pitch as u64 * height as u64);
    if end > 0x1_0000_0000 { FB = None; return false; }
    FB = Some(Framebuffer { address: address as u32, pitch, width, height, bpp,
        red_pos, red_size, green_pos, green_size, blue_pos, blue_size });
    true
}

pub fn info() -> Option<Framebuffer> { unsafe { FB } }

fn channel(value: u8, bits: u8) -> u32 {
    if bits == 0 { 0 } else {
        let max = (1u32 << bits.min(8)) - 1;
        ((value as u32 * max + 127) / 255) & max
    }
}

pub fn put_pixel(x: u32, y: u32, r: u8, g: u8, b: u8) -> bool {
    unsafe {
        let Some(fb) = FB else { return false; };
        if x >= fb.width || y >= fb.height { return false; }
        let pixel = (channel(r, fb.red_size) << fb.red_pos)
            | (channel(g, fb.green_size) << fb.green_pos)
            | (channel(b, fb.blue_size) << fb.blue_pos);
        let offset = y as usize * fb.pitch as usize + x as usize * ((fb.bpp as usize + 7) / 8);
        let ptr = (fb.address as *mut u8).add(offset);
        write_volatile(ptr, pixel as u8);
        write_volatile(ptr.add(1), (pixel >> 8) as u8);
        write_volatile(ptr.add(2), (pixel >> 16) as u8);
        if fb.bpp == 32 { write_volatile(ptr.add(3), (pixel >> 24) as u8); }
        true
    }
}

pub fn fill_rect(x: u32, y: u32, width: u32, height: u32, color: (u8,u8,u8)) {
    let Some(fb) = info() else { return; };
    let x1 = x.saturating_add(width).min(fb.width);
    let y1 = y.saturating_add(height).min(fb.height);
    for py in y.min(fb.height)..y1 {
        for px in x.min(fb.width)..x1 { let _ = put_pixel(px, py, color.0, color.1, color.2); }
    }
}
