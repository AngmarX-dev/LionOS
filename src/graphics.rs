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
    let red_mask = ((1u32 << red_size) - 1) << red_pos;
    let green_mask = ((1u32 << green_size) - 1) << green_pos;
    let blue_mask = ((1u32 << blue_size) - 1) << blue_pos;
    if (red_mask & green_mask) != 0 || (red_mask & blue_mask) != 0
        || (green_mask & blue_mask) != 0 { FB = None; return false; }
    let bytes = (bpp as u32 + 7) / 8;
    let Some(min_pitch) = width.checked_mul(bytes) else { FB = None; return false; };
    if pitch < min_pitch { FB = None; return false; }
    let Some(span) = (pitch as u64).checked_mul(height as u64) else { FB = None; return false; };
    let Some(end) = address.checked_add(span) else { FB = None; return false; };
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


/// Fill a rectangle using a vertical RGB gradient. Coordinates are clipped to
/// the framebuffer; zero-sized rectangles are no-ops.
pub fn gradient_rect(x: u32, y: u32, width: u32, height: u32,
    top: (u8, u8, u8), bottom: (u8, u8, u8)) {
    let Some(fb) = info() else { return; };
    let x0 = x.min(fb.width);
    let y0 = y.min(fb.height);
    let x1 = x.saturating_add(width).min(fb.width);
    let y1 = y.saturating_add(height).min(fb.height);
    if x0 >= x1 || y0 >= y1 { return; }
    let rows = (y1 - y0).saturating_sub(1).max(1);
    for py in y0..y1 {
        let step = py - y0;
        let mix = |a: u8, b: u8| -> u8 {
            ((a as u32 * (rows - step) + b as u32 * step) / rows) as u8
        };
        fill_rect(x0, py, x1 - x0, 1,
            (mix(top.0,bottom.0),mix(top.1,bottom.1),mix(top.2,bottom.2)));
    }
}

/// Draw a clipped rectangle outline without unsigned underflow.
pub fn rect_outline(x: u32, y: u32, width: u32, height: u32,
    color: (u8,u8,u8), thickness: u32) {
    if width == 0 || height == 0 || thickness == 0 { return; }
    let t = thickness.min(width).min(height);
    fill_rect(x,y,width,t,color);
    if height > t { fill_rect(x,y + height - t,width,t,color); }
    if height > 2*t {
        fill_rect(x,y + t,t,height - 2*t,color);
        if width > t { fill_rect(x + width - t,y + t,t,height - 2*t,color); }
    }
}

/// Filled rounded rectangle. Radius and bounds are clamped to the shape.
pub fn rounded_rect(x: u32, y: u32, width: u32, height: u32,
    radius: u32, color: (u8,u8,u8)) {
    let Some(fb) = info() else { return; };
    if width == 0 || height == 0 { return; }
    let x0=x.min(fb.width); let y0=y.min(fb.height);
    let x1=x.saturating_add(width).min(fb.width);
    let y1=y.saturating_add(height).min(fb.height);
    if x0>=x1 || y0>=y1 { return; }
    let r=radius.min(width/2).min(height/2);
    if r==0 { fill_rect(x0,y0,x1-x0,y1-y0,color); return; }
    let rr=(r as i64)*(r as i64);
    let left=(x+r) as i64; let right=(x+width-r-1) as i64;
    let top=(y+r) as i64; let bottom=(y+height-r-1) as i64;
    for py in y0..y1 {
        for px in x0..x1 {
            let cx=(px as i64).clamp(left,right);
            let cy=(py as i64).clamp(top,bottom);
            let dx=px as i64-cx; let dy=py as i64-cy;
            if dx*dx+dy*dy<=rr { let _=put_pixel(px,py,color.0,color.1,color.2); }
        }
    }
}

/// Bresenham line rasterizer; endpoints outside the framebuffer are safely
/// clipped by put_pixel. Work is bounded by the endpoint delta.
pub fn line(x0:i32,y0:i32,x1:i32,y1:i32,color:(u8,u8,u8)) {
    let mut x=x0; let mut y=y0;
    let dx=(x1-x0).abs(); let sx=if x0<x1 {1} else {-1};
    let dy=-(y1-y0).abs(); let sy=if y0<y1 {1} else {-1};
    let mut error=dx+dy;
    loop {
        if x>=0 && y>=0 { let _=put_pixel(x as u32,y as u32,color.0,color.1,color.2); }
        if x==x1 && y==y1 { break; }
        let twice=2*error;
        if twice>=dy { error+=dy; x+=sx; }
        if twice<=dx { error+=dx; y+=sy; }
    }
}

fn glyph5x7(byte:u8)->[u8;5] {
    match byte.to_ascii_uppercase() {
        b'A'=>[0x7E,0x11,0x11,0x11,0x7E], b'B'=>[0x7F,0x49,0x49,0x49,0x36],
        b'C'=>[0x3E,0x41,0x41,0x41,0x22], b'D'=>[0x7F,0x41,0x41,0x22,0x1C],
        b'E'=>[0x7F,0x49,0x49,0x49,0x41], b'F'=>[0x7F,0x09,0x09,0x09,0x01],
        b'G'=>[0x3E,0x41,0x49,0x49,0x7A], b'H'=>[0x7F,0x08,0x08,0x08,0x7F],
        b'I'=>[0x00,0x41,0x7F,0x41,0x00], b'J'=>[0x20,0x40,0x41,0x3F,0x01],
        b'K'=>[0x7F,0x08,0x14,0x22,0x41], b'L'=>[0x7F,0x40,0x40,0x40,0x40],
        b'M'=>[0x7F,0x02,0x0C,0x02,0x7F], b'N'=>[0x7F,0x04,0x08,0x10,0x7F],
        b'O'=>[0x3E,0x41,0x41,0x41,0x3E], b'P'=>[0x7F,0x09,0x09,0x09,0x06],
        b'Q'=>[0x3E,0x41,0x51,0x21,0x5E], b'R'=>[0x7F,0x09,0x19,0x29,0x46],
        b'S'=>[0x46,0x49,0x49,0x49,0x31], b'T'=>[0x01,0x01,0x7F,0x01,0x01],
        b'U'=>[0x3F,0x40,0x40,0x40,0x3F], b'V'=>[0x1F,0x20,0x40,0x20,0x1F],
        b'W'=>[0x3F,0x40,0x38,0x40,0x3F], b'X'=>[0x63,0x14,0x08,0x14,0x63],
        b'Y'=>[0x07,0x08,0x70,0x08,0x07], b'Z'=>[0x61,0x51,0x49,0x45,0x43],
        b'0'=>[0x3E,0x51,0x49,0x45,0x3E], b'1'=>[0x00,0x42,0x7F,0x40,0x00],
        b'2'=>[0x42,0x61,0x51,0x49,0x46], b'3'=>[0x21,0x41,0x45,0x4B,0x31],
        b'4'=>[0x18,0x14,0x12,0x7F,0x10], b'5'=>[0x27,0x45,0x45,0x45,0x39],
        b'6'=>[0x3C,0x4A,0x49,0x49,0x30], b'7'=>[0x01,0x71,0x09,0x05,0x03],
        b'8'=>[0x36,0x49,0x49,0x49,0x36], b'9'=>[0x06,0x49,0x49,0x29,0x1E],
        b'.'=>[0x00,0x60,0x60,0x00,0x00], b':'=>[0x00,0x36,0x36,0x00,0x00],
        b'-'=>[0x08,0x08,0x08,0x08,0x08], b'_'=>[0x40,0x40,0x40,0x40,0x40],
        b'/'=>[0x20,0x10,0x08,0x04,0x02], b'!'=>[0x00,0x00,0x5F,0x00,0x00],
        b'?'=>[0x02,0x01,0x51,0x09,0x06], b' '=>[0;5],
        _=>[0x7F,0x41,0x5D,0x41,0x7F],
    }
}

/// Draw compact ASCII text using a built-in 5x7 bitmap font. Supports newline,
/// clipping, and integer scale factors from 1 through 4.
pub fn draw_text(mut x:u32, mut y:u32, text:&str, color:(u8,u8,u8), scale:u32) {
    let scale=scale.clamp(1,4);
    let start_x=x;
    for byte in text.bytes() {
        if byte==b'\n' { x=start_x; y=y.saturating_add(9*scale); continue; }
        let glyph=glyph5x7(byte);
        for col in 0..5u32 {
            for row in 0..7u32 {
                if glyph[col as usize] & (1<<row)!=0 {
                    fill_rect(x+col*scale,y+row*scale,scale,scale,color);
                }
            }
        }
        x=x.saturating_add(6*scale);
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
