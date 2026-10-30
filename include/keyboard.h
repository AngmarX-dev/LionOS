#ifndef LIONOS_KEYBOARD_H
#define LIONOS_KEYBOARD_H

#include <stdint.h>

void keyboard_init(void);
void keyboard_handle_scancode(uint8_t scancode);
void keyboard_ps2_service(void);
void keyboard_irq_handler(void);
void keyboard_poll(void);
void keyboard_rearm_after_mouse_init(void);
void keyboard_handle_usb_report(const uint8_t *report, uint32_t length);
int keyboard_getchar(void);
uint32_t keyboard_available(void);
int keyboard_ps2_available(void);
uint32_t keyboard_scancode_count(void);
uint32_t keyboard_dropped_count(void);
uint8_t keyboard_last_scancode(void);
uint8_t keyboard_scancode_set(void);
uint8_t keyboard_ps2_status(void);
uint8_t keyboard_raw_input_seen(void);

#endif
