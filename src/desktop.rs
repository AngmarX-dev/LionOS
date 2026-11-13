//! Software desktop composition built on the RGB framebuffer renderer.
use crate::graphics;

#[derive(Clone, Copy, Debug)]
pub struct Rect { pub x:u32, pub y:u32, pub width:u32, pub height:u32 }
#[derive(Clone, Copy, Debug)]
pub struct Window { pub bounds:Rect, pub color:(u8,u8,u8), pub visible:bool }

pub fn draw_window(window: Window) {
    if !window.visible || window.bounds.width==0 || window.bounds.height==0 { return; }
    let b=window.bounds;
    graphics::rounded_rect(b.x.saturating_add(3),b.y.saturating_add(5),b.width,b.height,10,(7,10,18));
    graphics::rounded_rect(b.x,b.y,b.width,b.height,10,window.color);
    graphics::rect_outline(b.x,b.y,b.width,b.height,(76,91,116),1);
}

/// Compose a deterministic desktop scene using only framebuffer operations.
/// This is a software UI surface, not a GPU-accelerated compositor.
pub fn render_frame() {
    let Some(fb)=graphics::info() else { return; };
    graphics::gradient_rect(0,0,fb.width,fb.height,(20,31,52),(8,13,25));
    let top_h=fb.height.min(42);
    graphics::fill_rect(0,0,fb.width,top_h,(13,20,34));
    graphics::fill_rect(0,top_h.saturating_sub(1),fb.width,1,(53,77,110));

    let margin=24u32.min(fb.width/12);
    let panel_w=fb.width.saturating_sub(margin.saturating_mul(2));
    if panel_w>0 && fb.height>100 {
        let panel_h=fb.height.saturating_sub(42).min(190);
        draw_window(Window{
            bounds:Rect{x:margin,y:64,width:panel_w,height:panel_h},
            color:(27,41,64),visible:true,
        });
        graphics::draw_text(margin+18,82,"LIONOS DESKTOP",(232,240,252),2);
        graphics::draw_text(margin+18,108,"RUST SOFTWARE GRAPHICS ENGINE",(135,165,205),1);
        let card_y=145u32.min(fb.height.saturating_sub(12));
        let card_h=panel_h.saturating_sub(card_y.saturating_sub(64)+18).min(62);
        if card_h>0 {
            let gap=12;
            let card_w=panel_w.saturating_sub(36).saturating_sub(gap*2)/3;
            if card_w>0 {
                for i in 0..3 {
                    let cx=margin+18+i*(card_w+gap);
                    graphics::rounded_rect(cx,card_y,card_w,card_h,7,(35,54,82));
                    graphics::rect_outline(cx,card_y,card_w,card_h,(59,83,117),1);
                }
                graphics::draw_text(margin+30,card_y+12,"GRAPHICS",(174,205,245),1);
                graphics::draw_text(margin+30+card_w+gap,card_y+12,"INPUT",(174,205,245),1);
                graphics::draw_text(margin+30+2*(card_w+gap),card_y+12,"KERNEL",(174,205,245),1);
            }
        }
    }
    if fb.height>34 {
        let dock_w=fb.width.min(280);
        let dock_x=fb.width.saturating_sub(dock_w)/2;
        let dock_y=fb.height.saturating_sub(28);
        graphics::rounded_rect(dock_x,dock_y,dock_w,20,8,(30,43,64));
        graphics::rect_outline(dock_x,dock_y,dock_w,20,(72,91,122),1);
    }
}

static mut POINTER_LAST_X:u32=u32::MAX;
static mut POINTER_LAST_Y:u32=u32::MAX;
static mut POINTER_READY:bool=false;

/// Refresh the save-under pointer only when its coordinates have changed.
pub fn refresh_pointer() {
    let state=crate::mouse::snapshot();
    if !state.initialized || graphics::info().is_none() { return; }
    let x=state.x.max(0) as u32;
    let y=state.y.max(0) as u32;
    unsafe {
        if !POINTER_READY || x!=POINTER_LAST_X || y!=POINTER_LAST_Y {
            graphics::draw_cursor(x,y);
            POINTER_LAST_X=x;
            POINTER_LAST_Y=y;
            POINTER_READY=true;
        }
    }
}

static mut INPUT_TEXT: [u8; 32] = [0; 32];
static mut INPUT_TEXT_LEN: usize = 0;
static mut LAST_MOUSE_BUTTONS: u8 = 0;
static mut LAST_MOUSE_PACKETS: u32 = 0;

/// Drain keyboard transitions into a desktop status line and reflect mouse buttons.
pub fn poll_input() {
    let mut keyboard_changed = false;
    while let Some(event) = crate::input::pop_event() {
        if !event.pressed || event.extended { continue; }
        if let Some(byte) = crate::input::scancode_to_ascii(event.scancode) {
            unsafe {
                if byte == 8 {
                    if INPUT_TEXT_LEN != 0 {
                        INPUT_TEXT_LEN -= 1;
                        keyboard_changed = true;
                    }
                } else if (0x20..=0x7E).contains(&byte) {
                    keyboard_changed = true;
                    if INPUT_TEXT_LEN == 32 {
                        for index in 1..32 {
                            INPUT_TEXT[index - 1] = INPUT_TEXT[index];
                        }
                        INPUT_TEXT_LEN = 31;
                    }
                    INPUT_TEXT[INPUT_TEXT_LEN] = byte;
                    INPUT_TEXT_LEN += 1;
                }
            }
        }
    }

    let mouse = crate::mouse::snapshot();
    let mouse_changed = mouse.initialized
        && (mouse.buttons != unsafe { LAST_MOUSE_BUTTONS }
            || mouse.packets != unsafe { LAST_MOUSE_PACKETS });
    if !keyboard_changed && !mouse_changed { return; }

    unsafe {
        if mouse.initialized {
            LAST_MOUSE_BUTTONS = mouse.buttons;
            LAST_MOUSE_PACKETS = mouse.packets;
        }
        if let Some(fb) = graphics::info() {
            graphics::fill_rect(12, 12, fb.width.saturating_sub(24).min(420), 18, (13,20,34));
            graphics::draw_text(18, 16, "INPUT", (135,165,205), 1);
            let text_x = if mouse.initialized {
                let label = match mouse.buttons & 0x07 {
                    1 => "LEFT",
                    2 => "RIGHT",
                    4 => "MIDDLE",
                    _ => "MOVE",
                };
                graphics::draw_text(64, 16, label, (232,240,252), 1);
                112
            } else { 64 };
            let text_ptr = core::ptr::addr_of!(INPUT_TEXT).cast::<u8>();
            let text = core::slice::from_raw_parts(text_ptr, INPUT_TEXT_LEN);
            if let Ok(value) = core::str::from_utf8(text) {
                if !value.is_empty() {
                    graphics::draw_text(text_x, 16, value, (174,205,245), 1);
                }
            }
            let _ = graphics::present();
        }
    }
}


static mut HID_MOUSE_X: i32 = 0;
static mut HID_MOUSE_Y: i32 = 0;
static mut HID_MOUSE_BUTTONS: u8 = 0;
static mut HID_MOUSE_READY: bool = false;

unsafe fn render_input_status(device_label: &str) {
    if let Some(fb) = graphics::info() {
        graphics::fill_rect(12, 12, fb.width.saturating_sub(24).min(420), 18, (13,20,34));
        graphics::draw_text(18, 16, device_label, (135,165,205), 1);
        let text_ptr = core::ptr::addr_of!(INPUT_TEXT).cast::<u8>();
        let text = core::slice::from_raw_parts(text_ptr, INPUT_TEXT_LEN);
        if let Ok(value) = core::str::from_utf8(text) {
            if !value.is_empty() {
                graphics::draw_text(112, 16, value, (174,205,245), 1);
            }
        }
        let _ = graphics::present();
    }
}

/// Receive a decoded USB HID keyboard transition from the USB transfer layer.
pub fn accept_hid_key_transition(event: crate::usb_hid::KeyTransition) {
    if !event.pressed { return; }
    if let Some(byte) = crate::usb_hid::usage_to_ascii(event.usage) {
        unsafe {
            if byte == 8 {
                if INPUT_TEXT_LEN != 0 { INPUT_TEXT_LEN -= 1; }
            } else if (0x20..=0x7E).contains(&byte) {
                if INPUT_TEXT_LEN == 32 {
                    for index in 1..32 { INPUT_TEXT[index - 1] = INPUT_TEXT[index]; }
                    INPUT_TEXT_LEN = 31;
                }
                INPUT_TEXT[INPUT_TEXT_LEN] = byte;
                INPUT_TEXT_LEN += 1;
            } else { return; }
            render_input_status("USB HID");
        }
    }
}

/// Apply a decoded USB HID boot-mouse delta to the desktop pointer.
pub fn accept_hid_mouse_report(report: crate::usb_hid::BootMouseReport) {
    unsafe {
        if !HID_MOUSE_READY {
            if let Some(fb) = graphics::info() {
                HID_MOUSE_X = (fb.width.saturating_sub(1) / 2) as i32;
                HID_MOUSE_Y = (fb.height.saturating_sub(1) / 2) as i32;
            }
            HID_MOUSE_READY = true;
        }
        if let Some(fb) = graphics::info() {
            let max_x = fb.width.saturating_sub(12).min(i32::MAX as u32) as i32;
            let max_y = fb.height.saturating_sub(16).min(i32::MAX as u32) as i32;
            HID_MOUSE_X = HID_MOUSE_X.saturating_add(report.x as i32).clamp(0, max_x);
            HID_MOUSE_Y = HID_MOUSE_Y.saturating_sub(report.y as i32).clamp(0, max_y);
            graphics::draw_cursor(HID_MOUSE_X as u32, HID_MOUSE_Y as u32);
        }
        if report.buttons != HID_MOUSE_BUTTONS {
            HID_MOUSE_BUTTONS = report.buttons;
            render_input_status("USB MOUSE");
        }
    }
}
