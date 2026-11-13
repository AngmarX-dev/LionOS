//! Basic framebuffer compositor primitives; window manager and font renderer are pending.
use crate::graphics;
#[derive(Clone, Copy, Debug)]
pub struct Rect { pub x:u32, pub y:u32, pub width:u32, pub height:u32 }
#[derive(Clone, Copy, Debug)]
pub struct Window { pub bounds:Rect, pub color:(u8,u8,u8), pub visible:bool }
pub fn draw_window(window: Window) {
    if window.visible {
        graphics::fill_rect(window.bounds.x, window.bounds.y, window.bounds.width, window.bounds.height, window.color);
    }
}
