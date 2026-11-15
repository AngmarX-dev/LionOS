#include <stdint.h>
#include <stdarg.h>
#include <limits.h>
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
    uintptr_t da = (uintptr_t)d;
    uintptr_t sa = (uintptr_t)s;

    if (d == s || length == 0) return dst;
    if (da <= sa || da - sa >= length) {
        for (uint32_t i = 0; i < length; ++i) d[i] = s[i];
    } else {
        for (uint32_t i = length; i != 0; --i) d[i - 1u] = s[i - 1u];
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

char *strstr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    if (!*needle) return (char *)haystack;
    for (const char *p = haystack; *p; ++p) {
        const char *a = p, *b = needle;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b) return (char *)p;
    }
    return 0;
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

#define USER_HEAP_SIZE 32768u
#define USER_HEAP_ALIGN 8u
#define USER_HEAP_HEADER_SIZE 16u

struct user_heap_block {
    uint32_t size;
    uint32_t is_free;
    struct user_heap_block *next;
    uint32_t reserved;
};

static uint64_t user_heap[USER_HEAP_SIZE / sizeof(uint64_t)];
static struct user_heap_block *user_heap_head;
static int user_heap_initialized;

static void user_heap_init(void) {
    if (user_heap_initialized) return;
    user_heap_head = (struct user_heap_block *)user_heap;
    user_heap_head->size = USER_HEAP_SIZE - USER_HEAP_HEADER_SIZE;
    user_heap_head->is_free = 1u;
    user_heap_head->next = 0;
    user_heap_head->reserved = 0u;
    user_heap_initialized = 1;
}

void *malloc(uint32_t size) {
    if (size == 0u || size > UINT32_MAX - (USER_HEAP_ALIGN - 1u)) return 0;
    size = (size + USER_HEAP_ALIGN - 1u) & ~(USER_HEAP_ALIGN - 1u);
    user_heap_init();

    for (struct user_heap_block *block = user_heap_head; block; block = block->next) {
        if (!block->is_free || block->size < size) continue;

        uint32_t remainder = block->size - size;
        if (remainder >= USER_HEAP_HEADER_SIZE + USER_HEAP_ALIGN) {
            uint8_t *payload = (uint8_t *)block + USER_HEAP_HEADER_SIZE;
            struct user_heap_block *split =
                (struct user_heap_block *)(payload + size);
            split->size = remainder - USER_HEAP_HEADER_SIZE;
            split->is_free = 1u;
            split->next = block->next;
            split->reserved = 0u;
            block->next = split;
            block->size = size;
        }

        block->is_free = 0u;
        return (uint8_t *)block + USER_HEAP_HEADER_SIZE;
    }
    return 0;
}

void free(void *ptr) {
    if (!ptr || !user_heap_initialized) return;

    uintptr_t base = (uintptr_t)user_heap;
    uintptr_t address = (uintptr_t)ptr;
    uintptr_t end = base + USER_HEAP_SIZE;
    if (address < base + USER_HEAP_HEADER_SIZE || address >= end) return;

    struct user_heap_block *previous = 0;
    for (struct user_heap_block *block = user_heap_head; block; block = block->next) {
        uintptr_t payload = (uintptr_t)block + USER_HEAP_HEADER_SIZE;
        if (payload == address) {
            if (block->is_free) return;
            block->is_free = 1u;

            if (block->next && block->next->is_free) {
                block->size += USER_HEAP_HEADER_SIZE + block->next->size;
                block->next = block->next->next;
            }
            if (previous && previous->is_free) {
                previous->size += USER_HEAP_HEADER_SIZE + block->size;
                previous->next = block->next;
            }
            return;
        }
        previous = block;
    }
}

struct user_env_entry { char name[32]; char value[96]; uint8_t used; };
static struct user_env_entry user_env[16];
static int env_name_match(const char *a, const char *b) { return strcmp(a,b) == 0; }

char *getenv(const char *name) {
    if (!name) return 0;
    for (uint32_t i = 0; i < 16u; ++i)
        if (user_env[i].used && env_name_match(user_env[i].name,name)) return user_env[i].value;
    return 0;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || !value) return -1;
    for (uint32_t i = 0; i < 16u; ++i) {
        if (!user_env[i].used) continue;
        if (!env_name_match(user_env[i].name,name)) continue;
        if (!overwrite) return 0;
        uint32_t n = strlen(value); if (n >= sizeof(user_env[i].value)) return -1;
        memcpy(user_env[i].value,value,n+1u); return 0;
    }
    for (uint32_t i = 0; i < 16u; ++i) {
        if (user_env[i].used) continue;
        uint32_t n = strlen(name), v = strlen(value);
        if (n >= sizeof(user_env[i].name) || v >= sizeof(user_env[i].value)) return -1;
        memcpy(user_env[i].name,name,n+1u); memcpy(user_env[i].value,value,v+1u);
        user_env[i].used = 1u; return 0;
    }
    return -1;
}

int unsetenv(const char *name) {
    if (!name) return -1;
    for (uint32_t i = 0; i < 16u; ++i)
        if (user_env[i].used && env_name_match(user_env[i].name,name)) { user_env[i].used=0u; return 0; }
    return -1;
}

int atoi(const char *s) {
    uint32_t value = 0u;
    uint32_t limit;
    int negative = 0;

    if (!s) return 0;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' ||
           *s == '\f' || *s == '\v') ++s;
    if (*s == '-' || *s == '+') {
        negative = (*s == '-');
        ++s;
    }

    limit = negative ? (uint32_t)INT_MAX + 1u : (uint32_t)INT_MAX;
    while (*s >= '0' && *s <= '9') {
        uint32_t digit = (uint32_t)(*s - '0');
        if (value > (limit - digit) / 10u) {
            return negative ? INT_MIN : INT_MAX;
        }
        value = value * 10u + digit;
        ++s;
    }

    if (negative) {
        if (value == (uint32_t)INT_MAX + 1u) return INT_MIN;
        return -(int)value;
    }
    return (int)value;
}

void putchar(char c) {
    (void)lion_putc(c);
}

void puts(const char *s) {
    if (!s) return;
    (void)lion_write(s, strlen(s));
    putchar('\n');
}

static int print_u32(uint32_t value, uint32_t base) {
    char buf[32];
    uint32_t n = 0;
    int written = 0;
    const char digits[] = "0123456789abcdef";
    if (base < 2u || base > 16u) return -1;
    if (value == 0u) { putchar('0'); return 1; }
    while (value && n < sizeof(buf)) {
        buf[n++] = digits[value % base];
        value /= base;
    }
    while (n) {
        putchar(buf[--n]);
        ++written;
    }
    return written;
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
        else if (spec == 'u') { uint32_t v = va_arg(ap, uint32_t); written += print_u32(v, 10u); }
        else if (spec == 'd') { int32_t v = va_arg(ap, int32_t); if (v < 0) { putchar('-'); ++written; written += print_u32((uint32_t)(-(v + 1)) + 1u, 10u); } else written += print_u32((uint32_t)v, 10u); }
        else if (spec == 'x') { written += print_u32(va_arg(ap, uint32_t), 16u); }
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
