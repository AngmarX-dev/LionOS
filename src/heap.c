#include <stdint.h>
#include <stddef.h>
#include "heap.h"
#include "memory.h"

#define PAGE_SIZE 4096u
#define MAX_HEAP_ALLOCS 1024u

struct heap_alloc {
    void *base;
    uint32_t pages;
    uint8_t used;
};

static struct heap_alloc allocations[MAX_HEAP_ALLOCS];

void heap_init(void) {
    for (uint32_t i = 0; i < MAX_HEAP_ALLOCS; ++i) {
        allocations[i].base = 0;
        allocations[i].pages = 0;
        allocations[i].used = 0;
    }
}

void *kmalloc(size_t size) {
    if (size == 0 || size > ((size_t)0xFFFFFFFFu - (PAGE_SIZE - 1u)))
        return 0;

    uint32_t pages = (uint32_t)((size + PAGE_SIZE - 1u) / PAGE_SIZE);
    uint32_t slot = MAX_HEAP_ALLOCS;

    for (uint32_t i = 0; i < MAX_HEAP_ALLOCS; ++i) {
        if (!allocations[i].used) {
            slot = i;
            break;
        }
    }

    if (slot == MAX_HEAP_ALLOCS)
        return 0;

    void *base = page_alloc_contiguous(pages);
    if (!base)
        return 0;

    allocations[slot].base = base;
    allocations[slot].pages = pages;
    allocations[slot].used = 1;
    return base;
}

void kfree(void *ptr) {
    if (!ptr)
        return;

    for (uint32_t i = 0; i < MAX_HEAP_ALLOCS; ++i) {
        if (allocations[i].used && allocations[i].base == ptr) {
            uint32_t base = (uint32_t)(uintptr_t)allocations[i].base;
            for (uint32_t page = 0; page < allocations[i].pages; ++page)
                page_free((void *)(uintptr_t)(base + page * PAGE_SIZE));

            allocations[i].base = 0;
            allocations[i].pages = 0;
            allocations[i].used = 0;
            return;
        }
    }
}
