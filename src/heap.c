#include <stdint.h>
#include <stddef.h>
#include "heap.h"

#define HEAP_SIZE (64 * 1024)
static uint8_t heap[HEAP_SIZE] __attribute__((aligned(16)));
static size_t offset;

void heap_init(void) {
    offset = 0;
}

void *kmalloc(size_t size) {
    if (size == 0 || size > HEAP_SIZE - offset)
        return 0;

    void *ptr = &heap[offset];
    offset += (size + 15) & ~((size_t)15);
    return ptr;
}

void kfree(void *ptr) {
    (void)ptr;
    /* Simple bump allocator: reclamation will be added with block metadata. */
}
