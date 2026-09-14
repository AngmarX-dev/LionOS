#ifndef LIONOS_USER_LIBC_H
#define LIONOS_USER_LIBC_H

#include <stdint.h>
#include "user_api.h"

uint32_t strlen(const char *s);
void *memset(void *dst, int value, uint32_t length);
void *memcpy(void *dst, const void *src, uint32_t length);
void *memmove(void *dst, const void *src, uint32_t length);
int memcmp(const void *a, const void *b, uint32_t length);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, uint32_t length);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
int atoi(const char *s);
void puts(const char *s);
void putchar(char c);
int printf(const char *format, ...);

#endif
