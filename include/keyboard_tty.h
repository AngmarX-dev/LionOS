#ifndef LIONOS_KEYBOARD_TTY_H
#define LIONOS_KEYBOARD_TTY_H

#include <stdint.h>

/* Forward one translated byte to the currently claimed software TTY. */
int32_t keyboard_tty_forward_byte(uint8_t byte);

#endif
