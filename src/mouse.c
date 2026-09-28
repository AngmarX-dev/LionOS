#include <stdint.h>
#include "io.h"
#include "mouse.h"
#include "xhci.h"
#include "console.h"
#include "debug.h"

/* ---------------- PS/2 state ---------------- */
static volatile int32_t  ps2_x = 400, ps2_y = 300;
static volatile uint8_t  ps2_buttons = 0;
static volatile uint8_t  ps2_cycle = 0;
static volatile int      ps2_dx = 0, ps2_dy = 0;
static volatile int      ps2_initialized = 0;
static volatile int      ps2_cursor_visible = 1;

/* ---------------- USB state ---------------- */
static volatile int      usb_initialized = 0;
static volatile uint32_t usb_status = 0;   /* 0=off 1=ready 3=retrying */
static volatile uint32_t usb_retry_attempts = 0;
static volatile uint32_t usb_retry_frames = 0;
static volatile int32_t  usb_x = 400, usb_y = 300;
static volatile uint8_t  usb_buttons = 0;

/* ---- PS/2 helpers (skip these if you already have them) ---- */
static void ps2_wait_write(void){
    for(int i=0;i<100000;++i) if(!(inb(0x64) & 2)) return;
}
static void ps2_wait_read(void){
    for(int i=0;i<100000;++i) if(inb(0x64) & 1) return;
}
static void ps2_cmd(uint8_t v){
    ps2_wait_write(); outb(0x64, 0xD4);
    ps2_wait_write(); outb(0x60, v);
}
static uint8_t ps2_read(void){
    ps2_wait_read(); return inb(0x60);
}

/* IRQ12 handler (register this with your IDT / PIC if you haven't). */
void mouse_irq_handler(void){
    uint8_t st = inb(0x64);
    if(!(st & 0x20)) return;            /* not from mouse */
    uint8_t data = inb(0x60);

    switch(ps2_cycle){
        case 0: if(data & 0x08){ ps2_dx = data & 0x10 ? (int)(data | 0xFFFFFF00) : data; ps2_cycle = 1; } break;
        case 1: ps2_dy = data & 0x20 ? (int)(data | 0xFFFFFF00) : data; ps2_cycle = 2; break;
        case 2:
            ps2_buttons = data & 0x07;
            ps2_x += ps2_dx;
            ps2_y -= ps2_dy;
            if(ps2_x < 0) ps2_x = 0;
            if(ps2_y < 0) ps2_y = 0;
            if(ps2_x > 1023) ps2_x = 1023;
            if(ps2_y > 767)  ps2_y = 767;
            ps2_cycle = 0;
            break;
    }
}

void mouse_init(void){
    if(ps2_initialized) return;

    /* Enable aux device */
    ps2_wait_write(); outb(0x64, 0xA8);

    /* Enable IRQ12 in the PS/2 controller */
    ps2_wait_write(); outb(0x64, 0x20);
    ps2_wait_read();  uint8_t cfg = inb(0x60);
    cfg |= 0x02;                          /* IRQ12 enable */
    cfg &= ~0x20;                         /* clear mouse clock disable */
    ps2_wait_write(); outb(0x64, 0x60);
    ps2_wait_write(); outb(0x60, cfg);

    /* Mouse defaults */
    ps2_cmd(0xF6); (void)ps2_read();      /* set defaults */
    ps2_cmd(0xF4); (void)ps2_read();      /* enable data reporting */

    ps2_initialized = 1;
    console_write("[ OK ] PS/2 mouse initialized\n");
    debug_write("LIONOS:MOUSE-PS2-OK\n");
}

/* ---- USB integration ---- */
int mouse_usb_init(void){
    if(usb_initialized) return 0;
    usb_status = 3;                        /* retrying */
    if(xhci_mouse_init() != 0){
        usb_status = 0;                    /* unavailable */
        return -1;
    }
    usb_initialized = 1;
    usb_status = 1;                        /* ready */
    return 0;
}

void mouse_usb_retry(void){
    usb_retry_attempts = 0;
    usb_retry_frames = 0;
    usb_initialized = 0;
    usb_status = 3;
    (void)mouse_usb_init();
}

void mouse_poll(void){
    /* Always poll the PS/2 handler buffer if IRQs are on */
    if(ps2_initialized){
        /* nothing to do here — IRQ updates ps2_x/ps2_y */
    }

    /* USB polling */
    if(usb_initialized){
        int32_t dx = 0, dy = 0;
        uint8_t btn = 0;
        int r = xhci_mouse_poll(&dx, &dy, &btn);
        if(r == 1){
            usb_x += dx;
            usb_y += dy;
            if(usb_x < 0) usb_x = 0;
            if(usb_y < 0) usb_y = 0;
            if(usb_x > 1023) usb_x = 1023;
            if(usb_y > 767)  usb_y = 767;
            usb_buttons = btn;
        }
    }else if(usb_retry_attempts < 3u){
        /* Retry every ~30 frames (~0.5 s at 60 Hz) */
        usb_status = 3;
        if(++usb_retry_frames >= 30u){
            usb_retry_frames = 0;
            ++usb_retry_attempts;
            (void)mouse_usb_init();
        }
    }
}

/* ---- Public accessors ---- */
int32_t mouse_x(void){ return usb_initialized ? usb_x : ps2_x; }
int32_t mouse_y(void){ return usb_initialized ? usb_y : ps2_y; }
uint8_t mouse_buttons(void){ return usb_initialized ? usb_buttons : ps2_buttons; }

void mouse_set_cursor_visible(int visible){ ps2_cursor_visible = visible; }

uint32_t mouse_usb_status(void){ return usb_status; }

int mouse_debug_get(xhci_mouse_debug_info_t *out){
    return xhci_mouse_debug_get(out);
}
