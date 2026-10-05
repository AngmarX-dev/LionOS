#ifndef LIONOS_KEYBOARD_H
#define LIONOS_KEYBOARD_H

#include <stdint.h>

void keyboard_init(void);
void keyboard_handle_scancode(uint8_t scancode);
void keyboard_irq_handler(void);
void keyboard_poll(void);
void keyboard_rearm_after_mouse_init(void);
void keyboard_handle_usb_report(const uint8_t *report, uint32_t length);
int keyboard_getchar(void);
uint32_t keyboard_available(void);
int keyboard_ps2_available(void);

#endif
