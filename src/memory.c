#include <stdint.h>
#include <stddef.h>
#include "memory.h"

#define PAGE_SIZE 4096u
#define MAX_PAGES 65536u /* 256 MiB address space */
#define BITMAP_WORDS (MAX_PAGES / 32u)

#define MULTIBOOT2_TAG_END 0
#define MULTIBOOT2_TAG_MMAP 6
#define MULTIBOOT2_MEMORY_AVAILABLE 1

struct mb2_tag {
    uint32_t type;
    uint32_t size;
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

static uint32_t bitmap[BITMAP_WORDS];
static uint32_t total_pages;
static uint32_t free_pages;

static void mark_used(uint32_t page) {
    if (page >= MAX_PAGES) return;
    bitmap[page >> 5] |= 1u << (page & 31u);
}

static void mark_free(uint32_t page) {
    if (page >= MAX_PAGES) return;
    bitmap[page >> 5] &= ~(1u << (page & 31u));
}

static int is_free(uint32_t page) {
    if (page >= MAX_PAGES) return 0;
    return (bitmap[page >> 5] & (1u << (page & 31u))) == 0;
}

void memory_init(uint32_t multiboot_info) {
    for (uint32_t i = 0; i < BITMAP_WORDS; ++i)
        bitmap[i] = 0xFFFFFFFFu;

    total_pages = 0;
    free_pages = 0;

    uint8_t *tags = (uint8_t *)(uintptr_t)(multiboot_info + 8u);
    uint32_t total_size = *(uint32_t *)(uintptr_t)multiboot_info;
    uint8_t *end = (uint8_t *)(uintptr_t)(multiboot_info + total_size);

    while (tags < end) {
        struct mb2_tag *tag = (struct mb2_tag *)tags;
        if (tag->type == MULTIBOOT2_TAG_END)
            break;

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

    /* Keep low memory reserved for firmware, GRUB and the kernel image. */
    for (uint32_t page = 0; page < 256u; ++page) {
        if (is_free(page)) {
            mark_used(page);
            if (free_pages) --free_pages;
        }
    }
}

void *page_alloc(void) {
    for (uint32_t word = 0; word < BITMAP_WORDS; ++word) {
        uint32_t bits = bitmap[word];
        if (bits == 0xFFFFFFFFu) continue;

        for (uint32_t bit = 0; bit < 32u; ++bit) {
            if ((bits & (1u << bit)) == 0) {
                uint32_t page = word * 32u + bit;
                mark_used(page);
                if (free_pages) --free_pages;
                return (void *)(uintptr_t)(page * PAGE_SIZE);
            }
        }
    }
    return 0;
}

void page_free(void *page_ptr) {
    uint32_t address = (uint32_t)(uintptr_t)page_ptr;
    if ((address & (PAGE_SIZE - 1u)) != 0) return;

    uint32_t page = address / PAGE_SIZE;
    if (page >= MAX_PAGES || is_free(page)) return;

    /* Never release the first 1 MiB reserved by memory_init(). */
    if (page < 256u) return;

    mark_free(page);
    ++free_pages;
}

uint32_t memory_total_pages(void) {
    return total_pages;
}

uint32_t memory_free_pages(void) {
    return free_pages;
}
