#ifndef LIONOS_CONSOLE_H
#define LIONOS_CONSOLE_H

#include <stdint.h>

#define CONSOLE_COLOR_BLACK         0x00u
#define CONSOLE_COLOR_BLUE          0x01u
#define CONSOLE_COLOR_GREEN         0x02u
#define CONSOLE_COLOR_CYAN          0x03u
#define CONSOLE_COLOR_RED           0x04u
#define CONSOLE_COLOR_MAGENTA       0x05u
#define CONSOLE_COLOR_BROWN         0x06u
#define CONSOLE_COLOR_LIGHT_GRAY    0x07u
#define CONSOLE_COLOR_DARK_GRAY     0x08u
#define CONSOLE_COLOR_LIGHT_BLUE    0x09u
#define CONSOLE_COLOR_LIGHT_GREEN   0x0Au
#define CONSOLE_COLOR_LIGHT_CYAN    0x0Bu
#define CONSOLE_COLOR_LIGHT_RED     0x0Cu
#define CONSOLE_COLOR_LIGHT_MAGENTA 0x0Du
#define CONSOLE_COLOR_YELLOW        0x0Eu
#define CONSOLE_COLOR_WHITE         0x0Fu

void console_init(void);
void console_clear(void);
void console_putc(char c);
void console_write(const char *s);
void console_write_n(const char *s, uint32_t n);
void console_write_dec(uint32_t value);
void console_write_hex(uint32_t value);
void console_set_color(uint8_t fg);
uint8_t console_color(void);
void console_use_framebuffer(void);
uint32_t console_row(void);
uint32_t console_col(void);

#endif
