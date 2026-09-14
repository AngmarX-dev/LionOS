#ifndef LIONOS_HEAP_H
#define LIONOS_HEAP_H
#include <stddef.h>
void heap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);
#endif
