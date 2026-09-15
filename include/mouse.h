#ifndef LIONOS_MOUSE_H
#define LIONOS_MOUSE_H

#include <stdint.h>

int mouse_init(void);
void mouse_poll(void);
void mouse_show(void);
void mouse_hide(void);
uint32_t mouse_x(void);
uint32_t mouse_y(void);
uint8_t mouse_buttons(void);

#endif
