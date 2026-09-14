#include <stdint.h>
#include "paging.h"

#define PAGE_SIZE 4096u
#define PAGE_TABLE_COUNT 64u
#define IDENTITY_MAP_SIZE (PAGE_TABLE_COUNT * 1024u * PAGE_SIZE)

static uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t page_tables[PAGE_TABLE_COUNT][1024] __attribute__((aligned(4096)));

void paging_init(void) {
    for (uint32_t i = 0; i < 1024u; ++i) page_directory[i] = 0;

    for (uint32_t table = 0; table < PAGE_TABLE_COUNT; ++table) {
        for (uint32_t page = 0; page < 1024u; ++page) {
            uint32_t physical = (table * 1024u + page) * PAGE_SIZE;
            page_tables[table][page] = physical | 0x3u;
        }
        page_directory[table] = (uint32_t)page_tables[table] | 0x3u;
    }

    __asm__ volatile ("mov %0, %%cr3" : : "r"(page_directory) : "memory");

    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");

    (void)IDENTITY_MAP_SIZE;
}
