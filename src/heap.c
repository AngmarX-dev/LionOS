#include <stdint.h>
#include <stddef.h>
#include "heap.h"
#include "memory.h"
#include "spinlock.h"

#define PAGE_SIZE 4096u
#define MAX_HEAP_ALLOCS 1024u

struct heap_alloc {
    void *base;
    uint32_t pages;
    uint8_t used;
};

static struct heap_alloc allocations[MAX_HEAP_ALLOCS];
/* Lock order: heap_lock -> memory_lock; the PMM never calls back into heap. */
static struct spinlock heap_lock;
static uint8_t heap_initialized;

void heap_init(void)
{
    uint32_t irq = spinlock_irqsave_acquire(&heap_lock);
    if (!heap_initialized) {
        for (uint32_t i = 0; i < MAX_HEAP_ALLOCS; ++i) {
            allocations[i].base = 0;
            allocations[i].pages = 0;
            allocations[i].used = 0;
        }
        heap_initialized = 1u;
    }
    spinlock_irqrestore_release(&heap_lock, irq);
}

void *kmalloc(size_t size)
{
    uint32_t pages;
    uint32_t slot = MAX_HEAP_ALLOCS;
    void *base;

    if (size == 0u ||
        size > ((size_t)0xFFFFFFFFu - (PAGE_SIZE - 1u)))
        return 0;
    pages = (uint32_t)((size + PAGE_SIZE - 1u) / PAGE_SIZE);

    uint32_t irq = spinlock_irqsave_acquire(&heap_lock);
    if (!heap_initialized) {
        spinlock_irqrestore_release(&heap_lock, irq);
        return 0;
    }

    for (uint32_t i = 0; i < MAX_HEAP_ALLOCS; ++i) {
        if (!allocations[i].used) {
            slot = i;
            break;
        }
    }
    if (slot == MAX_HEAP_ALLOCS) {
        spinlock_irqrestore_release(&heap_lock, irq);
        return 0;
    }

    /* Reserve the table slot and PMM pages as one serialized operation. */
    base = page_alloc_contiguous(pages);
    if (!base) {
        spinlock_irqrestore_release(&heap_lock, irq);
        return 0;
    }
    allocations[slot].base = base;
    allocations[slot].pages = pages;
    allocations[slot].used = 1u;
    spinlock_irqrestore_release(&heap_lock, irq);
    return base;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    uint32_t irq = spinlock_irqsave_acquire(&heap_lock);
    if (!heap_initialized) {
        spinlock_irqrestore_release(&heap_lock, irq);
        return;
    }

    for (uint32_t i = 0; i < MAX_HEAP_ALLOCS; ++i) {
        if (allocations[i].used && allocations[i].base == ptr) {
            uintptr_t base = (uintptr_t)allocations[i].base;
            uint32_t pages = allocations[i].pages;
            /* Keep ownership metadata locked until PMM release is complete. */
            for (uint32_t page = 0; page < pages; ++page)
                page_free((void *)(base + (uintptr_t)page * PAGE_SIZE));
            allocations[i].base = 0;
            allocations[i].pages = 0;
            allocations[i].used = 0u;
            break;
        }
    }
    spinlock_irqrestore_release(&heap_lock, irq);
}
