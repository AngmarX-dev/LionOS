#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include "xhci.h"

/* Initialize the PS/2 mouse (and install IRQ12 handler). */
void mouse_init(void);

/* Try to bring up the USB (xHCI) mouse. Returns 0 on success. */
int  mouse_usb_init(void);

/* Force a fresh USB enumeration attempt (call after hot-plug). */
void mouse_usb_retry(void);

/* Called periodically from the GUI loop. */
void mouse_poll(void);

/* Called by the renderer each frame. */
void     mouse_set_cursor_visible(int visible);
int32_t  mouse_x(void);
int32_t  mouse_y(void);
uint8_t  mouse_buttons(void);

/* Diagnostics */
uint32_t mouse_usb_status(void);   /* 0=unavailable 1=ready 3=retrying */
int      mouse_debug_get(xhci_mouse_debug_info_t *out);

#endif
