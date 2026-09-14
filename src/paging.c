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
            /* Kernel identity map: present + writable, supervisor-only. */
            page_tables[table][page] = physical | 0x3u;
        }
        /* The PDEs are initially supervisor-only. User mappings opt in later. */
        page_directory[table] = (uint32_t)page_tables[table] | 0x3u;
    }

    __asm__ volatile ("mov %0, %%cr3" : : "r"(page_directory) : "memory");

    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");

    (void)IDENTITY_MAP_SIZE;
}

int paging_map_user_page(uint32_t virtual_address, uint32_t physical_address, uint32_t flags) {
    if ((virtual_address & (PAGE_SIZE - 1u)) != 0 ||
        (physical_address & (PAGE_SIZE - 1u)) != 0) {
        return -1;
    }

    uint32_t directory_index = virtual_address >> 22;
    uint32_t table_index = (virtual_address >> 12) & 0x3FFu;

    if (directory_index >= PAGE_TABLE_COUNT) return -1;

    /* Set U/S on the PDE, while individual PTEs still control access. */
    page_directory[directory_index] |= 0x4u;
    page_tables[directory_index][table_index] =
        (physical_address & 0xFFFFF000u) | (flags & 0x7u) | 0x5u;

    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
    return 0;
}
