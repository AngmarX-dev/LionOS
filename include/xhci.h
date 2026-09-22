#ifndef LIONOS_XHCI_H
#define LIONOS_XHCI_H

#include <stdint.h>

int xhci_mouse_init(void);
int xhci_mouse_poll(int32_t *dx, int32_t *dy, uint8_t *buttons);

#endif
