#include <stdint.h>
#include "memory.h"
#include "paging.h"

#define PAGE_SIZE 4096u
#define PAE_ROOT_ENTRIES 4u
#define PAE_PD_ENTRIES 512u
#define PAE_PT_ENTRIES 512u
#define IDENTITY_PT_COUNT 128u
#define KERNEL_MMIO_BASE 0xF0000000u
#define KERNEL_MMIO_PD_INDEX ((KERNEL_MMIO_BASE >> 21) & 0x1FFu)
#define KERNEL_MMIO_PT_COUNT 128u
#define USER_LIMIT 0xC0000000u
#define PROCESS_PAGING_PAGES 5u /* PDPT + four page-directory pages */

#define PTE_PRESENT 0x1ULL
#define PTE_WRITABLE 0x2ULL
#define PTE_USER 0x4ULL
#define PTE_FLAGS_MASK 0x7ULL
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/*
 * LionOS is still a 32-bit protected-mode kernel, but PAE lets the kernel
 * address physical memory above 4 GiB. This matters for modern UEFI/GOP
 * framebuffers, which are commonly placed above the 32-bit physical range.
 *
 * The normal kernel identity map remains 0..256 MiB so existing 32-bit
 * physical allocations stay unchanged. The F0000000..FFFFFFFF virtual range
 * is reserved for MMIO/framebuffer mappings and can point anywhere in the
 * 64-bit physical address space supported by PAE.
 */
static uint64_t page_root[PAE_ROOT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t kernel_pd[PAE_ROOT_ENTRIES][PAE_PD_ENTRIES] __attribute__((aligned(4096)));
static uint64_t identity_pt[IDENTITY_PT_COUNT][PAE_PT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t kernel_mmio_pt[KERNEL_MMIO_PT_COUNT][PAE_PT_ENTRIES] __attribute__((aligned(4096)));
static uint32_t current_directory;

static uint64_t *pdpt_ptr(uint32_t physical) {
    return (uint64_t *)(uintptr_t)physical;
}

static uint64_t *pd_ptr(uint32_t physical, uint32_t pdpt_index) {
    uint64_t *root = pdpt_ptr(physical);
    return (uint64_t *)(uintptr_t)(root[pdpt_index] & PTE_ADDR_MASK);
}

static uint64_t *pt_ptr(uint64_t entry) {
    return (uint64_t *)(uintptr_t)(entry & PTE_ADDR_MASK);
}

static void zero_words(uint64_t *buffer, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) buffer[i] = 0;
}

static void enable_pae_paging(void) {
    uint32_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 0x20u; /* CR4.PAE */
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");

    __asm__ volatile ("mov %0, %%cr3" : : "r"(current_directory) : "memory");

    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u; /* CR0.PG */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

void paging_init(void) {
    zero_words(page_root, PAE_ROOT_ENTRIES);
    for (uint32_t i = 0; i < PAE_ROOT_ENTRIES; ++i)
        zero_words(kernel_pd[i], PAE_PD_ENTRIES);
    for (uint32_t i = 0; i < IDENTITY_PT_COUNT; ++i)
        zero_words(identity_pt[i], PAE_PT_ENTRIES);
    for (uint32_t i = 0; i < KERNEL_MMIO_PT_COUNT; ++i)
        zero_words(kernel_mmio_pt[i], PAE_PT_ENTRIES);

    /* Identity-map the low 256 MiB with 4 KiB pages. */
    for (uint32_t table = 0; table < IDENTITY_PT_COUNT; ++table) {
        for (uint32_t page = 0; page < PAE_PT_ENTRIES; ++page) {
            uint64_t physical = ((uint64_t)table * PAE_PT_ENTRIES + page) * PAGE_SIZE;
            identity_pt[table][page] = physical | PTE_PRESENT | PTE_WRITABLE;
        }
        kernel_pd[0][table] =
            ((uint64_t)(uintptr_t)identity_pt[table] & PTE_ADDR_MASK) |
            PTE_PRESENT | PTE_WRITABLE;
    }

    /* Reserve the entire top 256 MiB of virtual space for MMIO/framebuffers. */
    for (uint32_t table = 0; table < KERNEL_MMIO_PT_COUNT; ++table) {
        kernel_pd[3][KERNEL_MMIO_PD_INDEX + table] =
            ((uint64_t)(uintptr_t)kernel_mmio_pt[table] & PTE_ADDR_MASK) |
            PTE_PRESENT | PTE_WRITABLE;
    }

    page_root[0] = ((uint64_t)(uintptr_t)kernel_pd[0] & PTE_ADDR_MASK) | PTE_PRESENT;
    page_root[1] = ((uint64_t)(uintptr_t)kernel_pd[1] & PTE_ADDR_MASK) | PTE_PRESENT;
    page_root[2] = ((uint64_t)(uintptr_t)kernel_pd[2] & PTE_ADDR_MASK) | PTE_PRESENT;
    page_root[3] = ((uint64_t)(uintptr_t)kernel_pd[3] & PTE_ADDR_MASK) | PTE_PRESENT;

    current_directory = (uint32_t)(uintptr_t)page_root;
    enable_pae_paging();
}

uint32_t paging_kernel_directory(void) {
    return (uint32_t)(uintptr_t)page_root;
}

int paging_map_kernel_page(uint32_t virtual_address, uint64_t physical_address, uint32_t flags) {
    if ((virtual_address & (PAGE_SIZE - 1u)) || (physical_address & (PAGE_SIZE - 1u))) return -1;
    if (virtual_address < KERNEL_MMIO_BASE) return -1;

    uint32_t pdpt_index = virtual_address >> 30;
    uint32_t pd_index = (virtual_address >> 21) & 0x1FFu;
    uint32_t pt_index = (virtual_address >> 12) & 0x1FFu;
    if (pdpt_index != 3u || pd_index < KERNEL_MMIO_PD_INDEX) return -1;

    uint32_t mmio_index = pd_index - KERNEL_MMIO_PD_INDEX;
    if (mmio_index >= KERNEL_MMIO_PT_COUNT) return -1;

    uint64_t *table = kernel_mmio_pt[mmio_index];
    table[pt_index] = (physical_address & PTE_ADDR_MASK) |
                      ((uint64_t)flags & PTE_FLAGS_MASK) |
                      PTE_PRESENT;

    if (current_directory == paging_kernel_directory())
        __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
    return 0;
}

uint32_t paging_create_address_space(void) {
    void *block = page_alloc_contiguous(PROCESS_PAGING_PAGES);
    if (!block) return 0;

    uint32_t base = (uint32_t)(uintptr_t)block;
    uint64_t *root = (uint64_t *)(uintptr_t)base;
    for (uint32_t i = 0; i < PROCESS_PAGING_PAGES * PAGE_SIZE / sizeof(uint64_t); ++i)
        root[i] = 0;

    for (uint32_t i = 0; i < PAE_ROOT_ENTRIES; ++i) {
        uint64_t *pd = (uint64_t *)(uintptr_t)(base + (i + 1u) * PAGE_SIZE);
        for (uint32_t j = 0; j < PAE_PD_ENTRIES; ++j)
            pd[j] = kernel_pd[i][j] & ~PTE_USER;
        root[i] = ((uint64_t)(uintptr_t)pd & PTE_ADDR_MASK) | PTE_PRESENT;
    }

    return base;
}

int paging_map_user_page_in(uint32_t pd_physical, uint32_t virtual_address,
                            uint32_t physical_address, uint32_t flags) {
    if (!pd_physical || virtual_address >= USER_LIMIT ||
        (virtual_address & (PAGE_SIZE - 1u)) || (physical_address & (PAGE_SIZE - 1u))) return -1;

    uint32_t pdpt_index = virtual_address >> 30;
    uint32_t pd_index = (virtual_address >> 21) & 0x1FFu;
    uint32_t pt_index = (virtual_address >> 12) & 0x1FFu;

    uint64_t *directory = pd_ptr(pd_physical, pdpt_index);
    if (!directory) return -1;

    uint64_t pde = directory[pd_index];
    uint64_t *table;
    if ((pde & PTE_PRESENT) && (pde & PTE_USER)) {
        table = pt_ptr(pde);
    } else {
        table = (uint64_t *)page_alloc();
        if (!table) return -1;
        zero_words(table, PAE_PT_ENTRIES);
        directory[pd_index] =
            ((uint64_t)(uintptr_t)table & PTE_ADDR_MASK) |
            PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    table[pt_index] = ((uint64_t)physical_address & PTE_ADDR_MASK) |
                      ((uint64_t)flags & PTE_FLAGS_MASK) |
                      PTE_PRESENT | PTE_USER;

    if (current_directory == pd_physical)
        __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
    return 0;
}

int paging_get_user_page(uint32_t pd_physical, uint32_t virtual_address,
                         uint32_t *physical_address, uint32_t *flags) {
    if (!pd_physical || virtual_address >= USER_LIMIT) return -1;

    uint32_t pdpt_index = virtual_address >> 30;
    uint32_t pd_index = (virtual_address >> 21) & 0x1FFu;
    uint32_t pt_index = (virtual_address >> 12) & 0x1FFu;

    uint64_t *directory = pd_ptr(pd_physical, pdpt_index);
    if (!directory) return -1;
    uint64_t pde = directory[pd_index];
    if (!(pde & PTE_PRESENT) || !(pde & PTE_USER)) return -1;

    uint64_t *table = pt_ptr(pde);
    uint64_t pte = table[pt_index];
    if (!(pte & PTE_PRESENT) || !(pte & PTE_USER)) return -1;

    if (physical_address) *physical_address = (uint32_t)(pte & PTE_ADDR_MASK);
    if (flags) *flags = (uint32_t)(pte & PTE_FLAGS_MASK);
    return 0;
}

void paging_destroy_address_space(uint32_t pd_physical) {
    if (!pd_physical || pd_physical == paging_kernel_directory()) return;

    uint32_t base = pd_physical;
    uint64_t *root = (uint64_t *)(uintptr_t)base;
    for (uint32_t i = 0; i < PAE_ROOT_ENTRIES; ++i) {
        uint64_t *directory = (uint64_t *)(uintptr_t)(base + (i + 1u) * PAGE_SIZE);
        for (uint32_t j = 0; j < PAE_PD_ENTRIES; ++j) {
            uint64_t pde = directory[j];
            /*
             * Kernel-shared mappings are supervisor-only and therefore have
             * PTE_USER clear. Only private userspace page tables are freed.
             */
            if ((pde & (PTE_PRESENT | PTE_USER)) == (PTE_PRESENT | PTE_USER))
                page_free((void *)(uintptr_t)(pde & PTE_ADDR_MASK));
        }
        root[i] = 0;
    }

    for (uint32_t i = 0; i < PROCESS_PAGING_PAGES; ++i)
        page_free((void *)(uintptr_t)(base + i * PAGE_SIZE));
}

void paging_switch_address_space(uint32_t pd_physical) {
    if (!pd_physical || pd_physical == current_directory) return;
    current_directory = pd_physical;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pd_physical) : "memory");
}

uint32_t paging_current_address_space(void) {
    return current_directory;
}
