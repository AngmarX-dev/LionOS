#include <stdint.h>

/*
 * Kernel-side freestanding memory/string primitives.
 *
 * The kernel is linked with -nostdlib, so compiler-generated calls to
 * standard C routines must be provided by LionOS itself.
 */

void *memset(void *dst, int value, uint32_t length) {
    uint8_t *p = (uint8_t *)dst;
    uint8_t v = (uint8_t)value;

    for (uint32_t i = 0; i < length; ++i)
        p[i] = v;

    return dst;
}

void *memcpy(void *dst, const void *src, uint32_t length) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    for (uint32_t i = 0; i < length; ++i)
        d[i] = s[i];

    return dst;
}

void *memmove(void *dst, const void *src, uint32_t length) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || length == 0)
        return dst;

    if (d < s) {
        for (uint32_t i = 0; i < length; ++i)
            d[i] = s[i];
    } else {
        for (uint32_t i = length; i != 0; --i)
            d[i - 1] = s[i - 1];
    }

    return dst;
}

int memcmp(const void *a, const void *b, uint32_t length) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;

    for (uint32_t i = 0; i < length; ++i) {
        if (x[i] != y[i])
            return (x[i] < y[i]) ? -1 : 1;
    }

    return 0;
}

uint32_t strlen(const char *s) {
    uint32_t n = 0;

    while (s[n] != '\0')
        ++n;

    return n;
}
