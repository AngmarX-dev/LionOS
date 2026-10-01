#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include "xhci.h"

void mouse_init(void);
void mouse_irq_handler(void);
int  mouse_usb_init(void);
void mouse_usb_retry(void);
void mouse_poll(void);

void     mouse_set_cursor_visible(int visible);
void     mouse_set_bounds(uint32_t max_x, uint32_t max_y);
int32_t  mouse_x(void);
int32_t  mouse_y(void);
uint8_t  mouse_buttons(void);

uint32_t    mouse_usb_status(void);
const char *mouse_usb_status_text(void);
int         mouse_debug_get(xhci_mouse_debug_info_t *out);

#endif
