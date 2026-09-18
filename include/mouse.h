#ifndef LIONOS_MOUSE_H
#define LIONOS_MOUSE_H

#include <stdint.h>

int mouse_init(void);
void mouse_poll(void);
void mouse_set_bounds(uint32_t width, uint32_t height);
uint32_t mouse_x(void);
uint32_t mouse_y(void);
uint8_t mouse_buttons(void);

#endif
