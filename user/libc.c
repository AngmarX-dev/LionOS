#include <stdint.h>
#include "user_libc.h"

uint32_t strlen(const char *s) {
    uint32_t n = 0;
    if (!s) return 0;
    while (s[n]) ++n;
    return n;
}

void *memset(void *dst, int value, uint32_t length) {
    uint8_t *p = (uint8_t *)dst;
    for (uint32_t i = 0; i < length; ++i) p[i] = (uint8_t)value;
    return dst;
}

void *memcpy(void *dst, const void *src, uint32_t length) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < length; ++i) d[i] = s[i];
    return dst;
}

int strcmp(const char *a, const char *b) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    while (*a && *a == *b) { ++a; ++b; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

void putchar(char c) {
    (void)lion_putc(c);
}

void puts(const char *s) {
    if (!s) return;
    (void)lion_write(s, strlen(s));
    putchar('\n');
}
