#include <stdint.h>
#include "memory.h"
#include "paging.h"

#define PAGE_SIZE 4096u
#define PAGE_TABLE_COUNT 64u
#define PAGE_ENTRIES 1024u
#define IDENTITY_MAP_SIZE (PAGE_TABLE_COUNT * PAGE_ENTRIES * PAGE_SIZE)

static uint32_t page_directory[PAGE_ENTRIES] __attribute__((aligned(4096)));
static uint32_t page_tables[PAGE_TABLE_COUNT][PAGE_ENTRIES] __attribute__((aligned(4096)));
static uint32_t current_directory;

static uint32_t *directory_ptr(uint32_t physical) {
    return (uint32_t *)(uintptr_t)physical;
}

void paging_init(void) {
    for (uint32_t i = 0; i < PAGE_ENTRIES; ++i) page_directory[i] = 0;

    for (uint32_t table = 0; table < PAGE_TABLE_COUNT; ++table) {
        for (uint32_t page = 0; page < PAGE_ENTRIES; ++page) {
            uint32_t physical = (table * PAGE_ENTRIES + page) * PAGE_SIZE;
            page_tables[table][page] = physical | 0x3u;
        }
        page_directory[table] = (uint32_t)(uintptr_t)page_tables[table] | 0x3u;
    }

    current_directory = (uint32_t)(uintptr_t)page_directory;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(current_directory) : "memory");

    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");

    (void)IDENTITY_MAP_SIZE;
}

uint32_t paging_kernel_directory(void) {
    return (uint32_t)(uintptr_t)page_directory;
}

uint32_t paging_create_address_space(void) {
    uint32_t *directory = (uint32_t *)page_alloc();
    if (!directory) return 0;

    for (uint32_t i = 0; i < PAGE_ENTRIES; ++i) directory[i] = 0;

    /* Kernel identity mappings are shared but remain supervisor-only. */
    for (uint32_t i = 0; i < PAGE_TABLE_COUNT; ++i)
        directory[i] = page_directory[i] & ~0x4u;

    return (uint32_t)(uintptr_t)directory;
}

int paging_map_user_page_in(uint32_t pd_physical, uint32_t virtual_address,
                            uint32_t physical_address, uint32_t flags) {
    if (pd_physical == 0 ||
        (virtual_address & (PAGE_SIZE - 1u)) != 0 ||
        (physical_address & (PAGE_SIZE - 1u)) != 0) return -1;

    uint32_t directory_index = virtual_address >> 22;
    uint32_t table_index = (virtual_address >> 12) & 0x3FFu;
    if (directory_index >= PAGE_TABLE_COUNT) return -1;

    uint32_t *directory = directory_ptr(pd_physical);
    uint32_t pde = directory[directory_index];
    uint32_t *table;

    /* Never promote a shared supervisor kernel table to user access. */
    if ((pde & 0x1u) && (pde & 0x4u)) {
        table = directory_ptr(pde & 0xFFFFF000u);
    } else {
        table = (uint32_t *)page_alloc();
        if (!table) return -1;
        for (uint32_t i = 0; i < PAGE_ENTRIES; ++i) table[i] = 0;
        directory[directory_index] = (uint32_t)(uintptr_t)table | 0x7u;
    }

    table[table_index] = (physical_address & 0xFFFFF000u) | (flags & 0x7u) | 0x5u;

    if (current_directory == pd_physical)
        __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
    return 0;
}

void paging_destroy_address_space(uint32_t pd_physical) {
    if (pd_physical == 0 || pd_physical == paging_kernel_directory()) return;

    uint32_t *directory = directory_ptr(pd_physical);

    /* User page tables have the U/S bit set; shared kernel tables do not. */
    for (uint32_t i = 0; i < PAGE_ENTRIES; ++i) {
        uint32_t pde = directory[i];
        if ((pde & 0x5u) == 0x5u) {
            uint32_t table = pde & 0xFFFFF000u;
            page_free((void *)(uintptr_t)table);
        }
    }

    page_free(directory);
}

void paging_switch_address_space(uint32_t pd_physical) {
    if (pd_physical == 0 || pd_physical == current_directory) return;
    current_directory = pd_physical;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pd_physical) : "memory");
}

uint32_t paging_current_address_space(void) {
    return current_directory;
}
