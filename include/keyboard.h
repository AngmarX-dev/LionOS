#ifndef LIONOS_KEYBOARD_H
#define LIONOS_KEYBOARD_H

#include <stdint.h>

void keyboard_init(void);
void keyboard_handle_scancode(uint8_t scancode);
void keyboard_poll(void);
int keyboard_getchar(void);
uint32_t keyboard_available(void);

#endif
