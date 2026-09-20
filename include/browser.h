#ifndef LIONOS_BROWSER_H
#define LIONOS_BROWSER_H
#include <stdint.h>
void browser_start(void);
void browser_close(void);
int browser_is_active(void);
void browser_key(int key);
void browser_mouse_click(uint32_t x, uint32_t y);
void browser_step(void);
void browser_render(void);
#endif
