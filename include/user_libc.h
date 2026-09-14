#ifndef LIONOS_USER_LIBC_H
#define LIONOS_USER_LIBC_H

#include <stdint.h>
#include "user_api.h"

uint32_t strlen(const char *s);
void *memset(void *dst, int value, uint32_t length);
void *memcpy(void *dst, const void *src, uint32_t length);
int strcmp(const char *a, const char *b);
void puts(const char *s);
void putchar(char c);

#endif
