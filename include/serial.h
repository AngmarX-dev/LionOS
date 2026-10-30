#ifndef LIONOS_SERIAL_H
#define LIONOS_SERIAL_H

#include <stdint.h>

#define SERIAL_COM1_BASE 0x03F8u
#define SERIAL_COM1_BAUD 115200u

int serial_init(void);
int serial_available(void);
void serial_putc(char c);
int serial_getchar(void);
uint32_t serial_rx_count(void);
uint32_t serial_tx_count(void);
uint32_t serial_error_count(void);

#endif
