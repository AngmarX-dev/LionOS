#include <stdint.h>
#include "io.h"
#include "mouse.h"
#include "xhci.h"
#include "console.h"
#include "debug.h"

/* ============================================================
 * PS/2 mouse state
 * ============================================================ */
static volatile int32_t  ps2_x = 400;
static volatile int32_t  ps2_y = 300;
static volatile uint8_t  ps2_buttons = 0;
#define MOUSE_GAIN 1
static int32_t mouse_scale_delta(int32_t d){return d*MOUSE_GAIN;}
static volatile uint8_t  ps2_cycle = 0;
static volatile int32_t  ps2_dx = 0;
static volatile int32_t  ps2_dy = 0;
static volatile int       ps2_initialized = 0;
static volatile int       cursor_visible = 1;

/* ============================================================
 * USB (xHCI) mouse state
 * ============================================================ */
static volatile int      usb_initialized = 0;
static volatile uint32_t usb_status = 0;          /* 0=off 1=ready 3=retrying */
static volatile uint32_t usb_retry_attempts = 0;
static volatile uint32_t usb_retry_frames = 0;
static volatile int32_t  usb_x = 400;
static volatile int32_t  usb_y = 300;
static volatile uint8_t  usb_buttons = 0;
static volatile uint32_t usb_recovery_cooldown = 0;
static int32_t cursor_smooth_x = 400;
static int32_t cursor_smooth_y = 300;
static int32_t smooth_step(int32_t current,int32_t target){
    int32_t d=target-current;
    if(d==0)return current;
    /* Keep smoothing, but converge faster so the pointer does not feel delayed. */
    int32_t step=(d*15)/16;
    if(step==0)step=(d>0)?1:-1;
    return current+step;
}

/* Cursor movement bounds — updated by mouse_set_bounds() */
static uint32_t cursor_max_x = 1023u;
static uint32_t cursor_max_y = 767u;

/* ============================================================
 * PS/2 low-level helpers
 * ============================================================ */
static void ps2_wait_write(void){
    for(uint32_t i = 0; i < 100000u; ++i){
        if((inb(0x64) & 2u) == 0u) return;
    }
}

static void ps2_wait_read(void){
    for(uint32_t i = 0; i < 100000u; ++i){
        if(inb(0x64) & 1u) return;
    }
}

static void ps2_cmd(uint8_t v){
    ps2_wait_write();
    outb(0x64, 0xD4u);      /* next byte goes to the auxiliary device */
    ps2_wait_write();
    outb(0x60, v);
}

static uint8_t ps2_read(void){
    ps2_wait_read();
    return inb(0x60);
}

/* IRQ12 handler — must be wired into the IDT by the kernel.
 * The firmware/PIC delivers byte 0 of the PS/2 packet to us;
 * we reassemble the 3-byte packet here. */
void mouse_irq_handler(void){
    uint8_t st = inb(0x64);
    if(!(st & 0x20u)) return;       /* not from the mouse */
    uint8_t data = inb(0x60);

    switch(ps2_cycle){
        case 0:
            if(data & 0x08u){       /* bit 3 must be 1 in byte 0 */
                ps2_dx = (data & 0x10u)
                    ? (int32_t)(data | 0xFFFFFF00u)
                    : (int32_t)data;
                ps2_buttons = data & 0x07u;
                ps2_cycle = 1;
            }
            break;
        case 1:
            ps2_dy = (data & 0x20u)
                ? (int32_t)(data | 0xFFFFFF00u)
                : (int32_t)data;
            ps2_cycle = 2;
            break;
        case 2:
            /* overflow bits — discard packet if set */
            if(!(data & 0xC0u)){
                ps2_x += mouse_scale_delta(ps2_dx);
                ps2_y -= mouse_scale_delta(ps2_dy);
                if(ps2_x < 0) ps2_x = 0;
                if(ps2_y < 0) ps2_y = 0;
                if(cursor_smooth_x > (int32_t)cursor_max_x) cursor_smooth_x = (int32_t)cursor_max_x;
    if(cursor_smooth_y > (int32_t)cursor_max_y) cursor_smooth_y = (int32_t)cursor_max_y;
    if(cursor_smooth_x < 0) cursor_smooth_x = 0;
    if(cursor_smooth_y < 0) cursor_smooth_y = 0;
    if(ps2_x > (int32_t)cursor_max_x) ps2_x = (int32_t)cursor_max_x;
                if(ps2_y > (int32_t)cursor_max_y) ps2_y = (int32_t)cursor_max_y;
            }
            ps2_cycle = 0;
            break;
    }
}

/* ============================================================
 * Public PS/2 init
 * ============================================================ */
void mouse_init(void){
    if(ps2_initialized) return;

    /* Enable auxiliary (mouse) device */
    ps2_wait_write(); outb(0x64, 0xA8u);

    /* Configure PS/2 controller: enable IRQ12, enable aux clock */
    ps2_wait_write(); outb(0x64, 0x20u);        /* read config byte */
    ps2_wait_read();
    uint8_t cfg = inb(0x60);
    cfg |= 0x02u;                               /* IRQ12 enable */
    cfg &= (uint8_t)~0x20u;                     /* aux clock enabled */
    ps2_wait_write(); outb(0x64, 0x60u);        /* write config byte */
    ps2_wait_write(); outb(0x60, cfg);

    /* Mouse defaults + enable reporting */
    ps2_cmd(0xF6u); (void)ps2_read();           /* set defaults */
    ps2_cmd(0xF4u); (void)ps2_read();           /* enable data */

    ps2_initialized = 1;
    console_write("[ OK ] PS/2 mouse initialized\n");
    debug_write("LIONOS:MOUSE-PS2-OK\n");
}

/* ============================================================
 * Public USB init / retry
 * ============================================================ */
int mouse_usb_init(void){
    if(usb_initialized) return 0;
    usb_status = 3u;                            /* RETRYING */
    if(xhci_mouse_init() != 0){
        usb_status = 0u;                        /* OFF */
        return -1;
    }
    usb_initialized = 1;
    usb_status = 1u;                            /* READY */
    usb_recovery_cooldown = 0u;
    return 0;
}

void mouse_usb_retry(void){
    usb_retry_attempts = 0;
    usb_retry_frames = 0;
    usb_initialized = 0;
    usb_status = 3u;
    usb_recovery_cooldown = 0u;
    (void)mouse_usb_init();
}

/* ============================================================
 * Poll — called every frame by the GUI loop
 * ============================================================ */
void mouse_poll(void){
    /* PS/2 state is updated by IRQ12 handler. Nothing to do here. */

    if(usb_initialized){
        for(uint32_t sample=0u;sample<4u;++sample){
            int32_t dx=0,dy=0;
            uint8_t btn=0;
            int r=xhci_mouse_poll(&dx,&dy,&btn);
            if(r<0){
                if(usb_recovery_cooldown==0u)
                    (void)xhci_mouse_recover();
                usb_recovery_cooldown=30u;
                break;
            }
            if(r!=1) break;

            usb_x+=mouse_scale_delta(dx);
            usb_y+=mouse_scale_delta(dy);
            if(usb_x<0)usb_x=0;
            if(usb_y<0)usb_y=0;
            if(usb_x>(int32_t)cursor_max_x)usb_x=(int32_t)cursor_max_x;
            if(usb_y>(int32_t)cursor_max_y)usb_y=(int32_t)cursor_max_y;
            usb_buttons=btn;
        }
    }else if(usb_retry_attempts<3u){
        usb_status=3u;
        if(++usb_retry_frames>=30u){
            usb_retry_frames=0u;
            ++usb_retry_attempts;
            (void)mouse_usb_init();
        }
    }

    if(usb_recovery_cooldown) --usb_recovery_cooldown;

    {
        int32_t tx=usb_initialized?usb_x:ps2_x;
        int32_t ty=usb_initialized?usb_y:ps2_y;
        cursor_smooth_x=smooth_step(cursor_smooth_x,tx);
        cursor_smooth_y=smooth_step(cursor_smooth_y,ty);
    }
}

/* ============================================================
 * Public accessors
 * ============================================================ */
int32_t mouse_x(void){ return cursor_smooth_x; }
int32_t mouse_y(void){ return cursor_smooth_y; }
uint8_t mouse_buttons(void){ return usb_initialized ? usb_buttons : ps2_buttons; }

void mouse_set_cursor_visible(int visible){ cursor_visible = visible; }
int  mouse_cursor_visible(void){ return cursor_visible; }

void mouse_set_bounds(uint32_t max_x, uint32_t max_y){
    cursor_max_x = max_x > 0u ? (max_x - 1u) : 0u;
    cursor_max_y = max_y > 0u ? (max_y - 1u) : 0u;
    if(ps2_x > (int32_t)cursor_max_x) ps2_x = (int32_t)cursor_max_x;
    if(ps2_y > (int32_t)cursor_max_y) ps2_y = (int32_t)cursor_max_y;
    if(usb_x > (int32_t)cursor_max_x) usb_x = (int32_t)cursor_max_x;
    if(usb_y > (int32_t)cursor_max_y) usb_y = (int32_t)cursor_max_y;
}

uint32_t mouse_usb_status(void){ return usb_status; }

const char *mouse_usb_status_text(void){
    if(usb_initialized)  return "ACTIVE";
    if(usb_status == 3u) return "RETRYING";
    if(usb_status == 1u) return "READY";
    return "OFF";
}

int mouse_debug_get(xhci_mouse_debug_info_t *out){
    return xhci_mouse_debug_get(out);
}
