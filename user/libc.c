#include <stdint.h>
#include <stdarg.h>
#include "user_libc.h"
#include "user_api.h"

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

void *memmove(void *dst, const void *src, uint32_t length) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || length == 0) return dst;
    if (d < s || d >= s + length) {
        for (uint32_t i = 0; i < length; ++i) d[i] = s[i];
    } else {
        for (uint32_t i = length; i != 0; --i) d[i - 1] = s[i - 1];
    }
    return dst;
}

int memcmp(const void *a, const void *b, uint32_t length) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (uint32_t i = 0; i < length; ++i) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

int strcmp(const char *a, const char *b) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    while (*a && *a == *b) { ++a; ++b; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, uint32_t length) {
    if (length == 0) return 0;
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    for (uint32_t i = 0; i < length; ++i) {
        uint8_t ac = (uint8_t)a[i];
        uint8_t bc = (uint8_t)b[i];
        if (ac != bc || ac == 0 || bc == 0) return (int)ac - (int)bc;
    }
    return 0;
}

char *strchr(const char *s, int c) {
    if (!s) return 0;
    while (*s) {
        if ((uint8_t)*s == (uint8_t)c) return (char *)s;
        ++s;
    }
    return (c == 0) ? (char *)s : 0;
}

char *strrchr(const char *s, int c) {
    const char *last = 0;
    if (!s) return 0;
    do {
        if ((uint8_t)*s == (uint8_t)c) last = s;
    } while (*s++);
    return (char *)last;
}

int atoi(const char *s) {
    int sign = 1;
    int value = 0;
    if (!s) return 0;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
    if (*s == '-' || *s == '+') {
        if (*s == '-') sign = -1;
        ++s;
    }
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        ++s;
    }
    return value * sign;
}

void putchar(char c) {
    (void)lion_putc(c);
}

void puts(const char *s) {
    if (!s) return;
    (void)lion_write(s, strlen(s));
    putchar('\n');
}

static void print_u32(uint32_t value, uint32_t base) {
    char buf[11];
    uint32_t n = 0;
    const char digits[] = "0123456789abcdef";
    if (value == 0) { putchar('0'); return; }
    while (value && n < sizeof(buf)) { buf[n++] = digits[value % base]; value /= base; }
    while (n) putchar(buf[--n]);
}

int printf(const char *format, ...) {
    if (!format) return -1;
    va_list ap;
    va_start(ap, format);
    int written = 0;
    for (uint32_t i = 0; format[i]; ++i) {
        if (format[i] != '%') { putchar(format[i]); ++written; continue; }
        char spec = format[++i];
        if (!spec) break;
        if (spec == '%') { putchar('%'); ++written; }
        else if (spec == 'c') { putchar((char)va_arg(ap, int)); ++written; }
        else if (spec == 's') { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; uint32_t n = strlen(s); (void)lion_write(s, n); written += (int)n; }
        else if (spec == 'u') { uint32_t v = va_arg(ap, uint32_t); print_u32(v, 10u); }
        else if (spec == 'd') { int32_t v = va_arg(ap, int32_t); if (v < 0) { putchar('-'); ++written; print_u32((uint32_t)(-(v + 1)) + 1u, 10u); } else print_u32((uint32_t)v, 10u); }
        else if (spec == 'x') { print_u32(va_arg(ap, uint32_t), 16u); }
        else { putchar('%'); putchar(spec); written += 2; }
    }
    va_end(ap);
    return written;
}

/* Exported bridge for crt0.S. user_api.h's lion_exit_code is static inline. */
void lion_exit_code_bridge(int code) {
    lion_exit_code((uint32_t)code);
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
