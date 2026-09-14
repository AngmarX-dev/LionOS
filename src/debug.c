#include "debug.h"
#include "io.h"

#define DEBUGCON_PORT 0xE9u

void debug_putc(char c) {
    outb(DEBUGCON_PORT, (uint8_t)c);
}

void debug_write(const char *s) {
    while (*s) debug_putc(*s++);
}
