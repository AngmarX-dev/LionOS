#ifndef LIONOS_CONSOLE_H
#define LIONOS_CONSOLE_H

#include <stdint.h>

void console_init(void);
void console_clear(void);
void console_putc(char c);
void console_write(const char *s);
void console_write_n(const char *s, uint32_t n);
void console_write_dec(uint32_t value);
void console_write_hex(uint32_t value);
uint32_t console_row(void);
uint32_t console_col(void);

#endif
