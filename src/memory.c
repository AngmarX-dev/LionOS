#include <stdint.h>
#include "memory.h"

#define PAGE_SIZE 4096u
#define MAX_PAGES 65536u
#define BITMAP_WORDS (MAX_PAGES / 32u)
#define RESERVED_PAGES 1024u

#define MULTIBOOT2_TAG_END 0
#define MULTIBOOT2_TAG_MODULE 3
#define MULTIBOOT2_TAG_MMAP 6
#define MULTIBOOT2_MEMORY_AVAILABLE 1

struct mb2_tag { uint32_t type; uint32_t size; };
struct mb2_module_tag {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char string[0];
};
struct mb2_mmap_tag {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
};
struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
};

extern uint8_t _kernel_start;
extern uint8_t _kernel_end;

static uint32_t bitmap[BITMAP_WORDS];
static uint32_t total_pages;
static uint32_t free_pages;

static void mark_used(uint32_t page) {
    if (page < MAX_PAGES) bitmap[page >> 5] |= 1u << (page & 31u);
}

static void mark_free(uint32_t page) {
    if (page < MAX_PAGES) bitmap[page >> 5] &= ~(1u << (page & 31u));
}

static int is_free(uint32_t page) {
    if (page >= MAX_PAGES) return 0;
    return (bitmap[page >> 5] & (1u << (page & 31u))) == 0;
}

static void reserve_range(uint64_t start, uint64_t end) {
    if (end <= start) return;

    uint64_t aligned_start = start & ~(uint64_t)(PAGE_SIZE - 1u);
    uint64_t aligned_end = (end + PAGE_SIZE - 1u) & ~(uint64_t)(PAGE_SIZE - 1u);

    for (uint64_t addr = aligned_start; addr < aligned_end; addr += PAGE_SIZE) {
        uint32_t page = (uint32_t)(addr / PAGE_SIZE);
        if (page < MAX_PAGES && is_free(page)) {
            mark_used(page);
            if (free_pages) --free_pages;
        }
    }
}

void memory_init(uint32_t multiboot_info) {
    for (uint32_t i = 0; i < BITMAP_WORDS; ++i) bitmap[i] = 0xFFFFFFFFu;
    total_pages = 0;
    free_pages = 0;

    uint8_t *info = (uint8_t *)(uintptr_t)multiboot_info;
    uint32_t total_size = *(uint32_t *)(uintptr_t)multiboot_info;
    uint8_t *tags = info + 8u;
    uint8_t *end = info + total_size;

    while (tags + sizeof(struct mb2_tag) <= end) {
        struct mb2_tag *tag = (struct mb2_tag *)tags;
        if (tag->type == MULTIBOOT2_TAG_END) break;

        if (tag->type == MULTIBOOT2_TAG_MMAP) {
            struct mb2_mmap_tag *mmap = (struct mb2_mmap_tag *)tag;
            uint8_t *p = tags + sizeof(struct mb2_mmap_tag);
            uint8_t *mmap_end = tags + mmap->size;

            while (p + mmap->entry_size <= mmap_end) {
                struct mb2_mmap_entry *entry = (struct mb2_mmap_entry *)p;
                if (entry->type == MULTIBOOT2_MEMORY_AVAILABLE) {
                    uint64_t start = (entry->addr + PAGE_SIZE - 1u) & ~(uint64_t)(PAGE_SIZE - 1u);
                    uint64_t finish = (entry->addr + entry->len) & ~(uint64_t)(PAGE_SIZE - 1u);
                    for (uint64_t addr = start; addr < finish; addr += PAGE_SIZE) {
                        uint32_t page = (uint32_t)(addr / PAGE_SIZE);
                        if (page < MAX_PAGES && is_free(page)) {
                            mark_free(page);
                            ++free_pages;
                            if (page + 1u > total_pages) total_pages = page + 1u;
                        }
                    }
                }
                p += mmap->entry_size;
            }
        }

        tags += (tag->size + 7u) & ~7u;
    }

    /* Keep bootstrap memory reserved. */
    reserve_range(0, RESERVED_PAGES * PAGE_SIZE);

    /* Reserve the linked kernel image even if it grows beyond the bootstrap area. */
    reserve_range((uint32_t)(uintptr_t)&_kernel_start,
                  (uint32_t)(uintptr_t)&_kernel_end);

    /* Reserve the Multiboot information structure itself. */
    reserve_range(multiboot_info, (uint64_t)multiboot_info + total_size);

    /* Reserve Multiboot modules so the PMM cannot hand their pages to the kernel. */
    tags = info + 8u;
    while (tags + sizeof(struct mb2_tag) <= end) {
        struct mb2_tag *tag = (struct mb2_tag *)tags;
        if (tag->type == MULTIBOOT2_TAG_END) break;

        if (tag->type == MULTIBOOT2_TAG_MODULE && tag->size >= 16u) {
            struct mb2_module_tag *module = (struct mb2_module_tag *)tag;
            reserve_range(module->mod_start, module->mod_end);
        }

        tags += (tag->size + 7u) & ~7u;
    }
}

void *page_alloc_contiguous(uint32_t count) {
    if (count == 0 || count > MAX_PAGES - RESERVED_PAGES)
        return 0;

    uint32_t run = 0;
    uint32_t start = RESERVED_PAGES;
    for (uint32_t page = RESERVED_PAGES; page < MAX_PAGES; ++page) {
        if (is_free(page)) {
            if (run == 0) start = page;
            ++run;
            if (run == count) {
                for (uint32_t i = 0; i < count; ++i)
                    mark_used(start + i);
                free_pages -= count;
                return (void *)(uintptr_t)(start * PAGE_SIZE);
            }
        } else {
            run = 0;
        }
    }
    return 0;
}

void *page_alloc(void) {
    return page_alloc_contiguous(1);
}

void page_free(void *page_ptr) {
    uint32_t address = (uint32_t)(uintptr_t)page_ptr;
    if ((address & (PAGE_SIZE - 1u)) != 0) return;
    uint32_t page = address / PAGE_SIZE;
    if (page < RESERVED_PAGES || page >= MAX_PAGES || is_free(page)) return;
    mark_free(page);
    ++free_pages;
}

uint32_t memory_total_pages(void) { return total_pages; }
uint32_t memory_free_pages(void) { return free_pages; }
