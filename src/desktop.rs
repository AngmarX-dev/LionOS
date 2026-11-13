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

static mut POINTER_LAST_X: u32 = u32::MAX;
static mut POINTER_LAST_Y: u32 = u32::MAX;
static mut POINTER_READY: bool = false;

/// Refresh the desktop pointer after input interrupts; the renderer restores
/// the previous save-under region before drawing the new pointer position.
pub fn refresh_pointer() {
    let state = crate::mouse::snapshot();
    if !state.initialized { return; }
    if graphics::info().is_none() { return; }
    let x = state.x.max(0) as u32;
    let y = state.y.max(0) as u32;
    unsafe {
        if !POINTER_READY || x != POINTER_LAST_X || y != POINTER_LAST_Y {
            graphics::draw_cursor(x, y);
            POINTER_LAST_X = x;
            POINTER_LAST_Y = y;
            POINTER_READY = true;
        }
    }
}
