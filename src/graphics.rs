//! Conservative Multiboot2 framebuffer renderer. Supports packed RGB 24/32 bpp.
use core::ptr::{read_volatile, write_volatile};

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
        || red_size == 0 || green_size == 0 || blue_size == 0
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

const CURSOR_WIDTH: usize = 12;
const CURSOR_HEIGHT: usize = 16;
const CURSOR_ROWS: [u16; CURSOR_HEIGHT] = [
    0x800, 0xC00, 0xA00, 0x980, 0x8C0, 0x860, 0x830, 0x818,
    0x80C, 0x806, 0x803, 0x87E, 0x8C0, 0xA60, 0xD30, 0xE18,
];
static mut CURSOR_VISIBLE: bool = false;
static mut CURSOR_X: u32 = 0;
static mut CURSOR_Y: u32 = 0;
static mut CURSOR_SAVE: [u32; CURSOR_WIDTH * CURSOR_HEIGHT] = [0; CURSOR_WIDTH * CURSOR_HEIGHT];

unsafe fn read_raw_pixel(fb: Framebuffer, x: u32, y: u32) -> u32 {
    let bytes = (fb.bpp as usize + 7) / 8;
    let ptr = (fb.address as *const u8).add(y as usize * fb.pitch as usize + x as usize * bytes);
    let mut value = read_volatile(ptr) as u32;
    value |= (read_volatile(ptr.add(1)) as u32) << 8;
    value |= (read_volatile(ptr.add(2)) as u32) << 16;
    if bytes == 4 { value |= (read_volatile(ptr.add(3)) as u32) << 24; }
    value
}

unsafe fn write_raw_pixel(fb: Framebuffer, x: u32, y: u32, value: u32) {
    let bytes = (fb.bpp as usize + 7) / 8;
    let ptr = (fb.address as *mut u8).add(y as usize * fb.pitch as usize + x as usize * bytes);
    write_volatile(ptr, value as u8);
    write_volatile(ptr.add(1), (value >> 8) as u8);
    write_volatile(ptr.add(2), (value >> 16) as u8);
    if bytes == 4 { write_volatile(ptr.add(3), (value >> 24) as u8); }
}

/// Draw a software pointer with save-under restoration so moving it does not
/// leave trails or permanently overwrite the desktop beneath it.
pub fn draw_cursor(x: u32, y: u32) {
    unsafe {
        let Some(fb) = FB else { return; };
        let save = core::ptr::addr_of_mut!(CURSOR_SAVE);
        if CURSOR_VISIBLE {
            for row in 0..CURSOR_HEIGHT {
                for col in 0..CURSOR_WIDTH {
                    if CURSOR_ROWS[row] & (1 << (CURSOR_WIDTH - 1 - col)) != 0 {
                        let px = CURSOR_X.saturating_add(col as u32);
                        let py = CURSOR_Y.saturating_add(row as u32);
                        if px < fb.width && py < fb.height {
                            write_raw_pixel(fb, px, py, (*save)[row * CURSOR_WIDTH + col]);
                        }
                    }
                }
            }
        }

        let max_x = fb.width.saturating_sub(CURSOR_WIDTH as u32);
        let max_y = fb.height.saturating_sub(CURSOR_HEIGHT as u32);
        let origin_x = x.min(max_x);
        let origin_y = y.min(max_y);
        for row in 0..CURSOR_HEIGHT {
            for col in 0..CURSOR_WIDTH {
                if CURSOR_ROWS[row] & (1 << (CURSOR_WIDTH - 1 - col)) != 0 {
                    let px = origin_x + col as u32;
                    let py = origin_y + row as u32;
                    if px < fb.width && py < fb.height {
                        (*save)[row * CURSOR_WIDTH + col] = read_raw_pixel(fb, px, py);
                        let _ = put_pixel(px, py, 8, 10, 16);
                    }
                }
            }
        }
        // Bright inner edge for contrast on both light and dark backgrounds.
        for row in 2..11 {
            for col in 1..CURSOR_WIDTH {
                if CURSOR_ROWS[row] & (1 << (CURSOR_WIDTH - 1 - col)) != 0 {
                    let px = origin_x + col as u32;
                    let py = origin_y + row as u32;
                    if px < fb.width && py < fb.height {
                        let _ = put_pixel(px, py, 245, 247, 252);
                    }
                }
            }
        }
        CURSOR_X = origin_x;
        CURSOR_Y = origin_y;
        CURSOR_VISIBLE = true;
    }
}
