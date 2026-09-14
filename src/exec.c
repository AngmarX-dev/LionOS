#include <stdint.h>
#include "elf.h"
#include "exec.h"
#include "memory.h"
#include "paging.h"
#include "process.h"
#include "ramfs.h"

#define USER_BASE 0x00400000u
#define USER_LIMIT 0xC0000000u
#define USER_STACK_PAGE 0xBFFFF000u
#define USER_STACK_TOP 0xC0000000u
#define PAGE_SIZE 4096u
#define MAX_EXEC_PAGES LIONOS_PROCESS_MAX_USER_PAGES

static uint32_t align_down(uint32_t x) { return x & ~(PAGE_SIZE - 1u); }
static uint32_t align_up(uint32_t x) {
    return (x + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
}

int exec_validate_image(const uint8_t *image, uint32_t size, uint32_t *entry, uint32_t *stack_top) {
    if (elf32_validate(image, size, entry) != 0) return -1;
    const struct elf32_header *h = (const struct elf32_header *)image;
    uint32_t load_count = 0;

    for (uint32_t i = 0; i < h->phnum; ++i) {
        const struct elf32_phdr *p = (const struct elf32_phdr *)(image + h->phoff + i * h->phentsize);
        if (p->type != PT_LOAD || p->memsz == 0) continue;
        ++load_count;
        if (p->vaddr < USER_BASE || p->vaddr >= USER_LIMIT) return -1;
        if (p->memsz > USER_LIMIT - p->vaddr) return -1;
        if (p->filesz && p->offset > 0xFFFFFFFFu - p->filesz) return -1;
        if (p->offset + p->filesz > 0xFFFFFFFFu) return -1;
        uint32_t end = p->vaddr + p->memsz;
        if (end < p->vaddr || end > USER_LIMIT) return -1;
        uint32_t first = align_down(p->vaddr);
        uint32_t last = align_up(end);
        if (last < first || (last - first) / PAGE_SIZE > MAX_EXEC_PAGES) return -1;
        if (first <= USER_STACK_PAGE && USER_STACK_PAGE < last) return -1;
        if ((p->flags & 0x7u) == 0) return -1;
    }

    if (!load_count || *entry < USER_BASE || *entry >= USER_LIMIT || *entry == USER_STACK_PAGE) return -1;
    *stack_top = USER_STACK_TOP;
    return 0;
}

static int find_page(const uint32_t *vas, uint32_t count, uint32_t va) {
    for (uint32_t i = 0; i < count; ++i) if (vas[i] == va) return (int)i;
    return -1;
}

int exec_run_file(const char *name) {
    const uint8_t *image = (const uint8_t *)ramfs_data(name);
    uint32_t size = ramfs_size(name);
    uint32_t entry, stack_top;
    if (!image || !size || exec_validate_image(image, size, &entry, &stack_top) != 0) return -1;

    const struct elf32_header *h = (const struct elf32_header *)image;
    uint32_t page_vas[MAX_EXEC_PAGES];
    uint32_t page_phys[MAX_EXEC_PAGES];
    uint32_t page_flags[MAX_EXEC_PAGES];
    uint32_t page_count = 0;
    for (uint32_t i = 0; i < MAX_EXEC_PAGES; ++i) {
        page_vas[i] = 0; page_phys[i] = 0; page_flags[i] = 0;
    }

    /* First pass: determine every unique page required by PT_LOAD segments. */
    for (uint32_t i = 0; i < h->phnum; ++i) {
        const struct elf32_phdr *p = (const struct elf32_phdr *)(image + h->phoff + i * h->phentsize);
        if (p->type != PT_LOAD || p->memsz == 0) continue;
        uint32_t first = align_down(p->vaddr);
        uint32_t last = align_up(p->vaddr + p->memsz);
        for (uint32_t va = first; va < last; va += PAGE_SIZE) {
            int index = find_page(page_vas, page_count, va);
            if (index < 0) {
                if (page_count >= MAX_EXEC_PAGES) goto fail;
                index = (int)page_count++;
                page_vas[index] = va;
                page_phys[index] = (uint32_t)(uintptr_t)page_alloc();
                if (!page_phys[index]) goto fail;
                page_flags[index] = 0x1u;
                for (uint32_t b = 0; b < PAGE_SIZE; ++b) ((uint8_t *)(uintptr_t)page_phys[index])[b] = 0;
            }
            if (p->flags & 0x2u) page_flags[index] |= 0x2u;
        }
    }

    /* Second pass: copy initialized bytes. BSS stays zero from the first pass. */
    for (uint32_t i = 0; i < h->phnum; ++i) {
        const struct elf32_phdr *p = (const struct elf32_phdr *)(image + h->phoff + i * h->phentsize);
        if (p->type != PT_LOAD || !p->filesz) continue;
        for (uint32_t n = 0; n < p->filesz; ++n) {
            uint32_t va = p->vaddr + n;
            int index = find_page(page_vas, page_count, align_down(va));
            if (index < 0) goto fail;
            ((uint8_t *)(uintptr_t)page_phys[index])[va & (PAGE_SIZE - 1u)] = image[p->offset + n];
        }
    }

    /* Allocate and clear the initial user stack. */
    uint32_t stack_phys = (uint32_t)(uintptr_t)page_alloc();
    if (!stack_phys) goto fail;
    for (uint32_t b = 0; b < PAGE_SIZE; ++b) ((uint8_t *)(uintptr_t)stack_phys)[b] = 0;

    if (page_count >= MAX_EXEC_PAGES) { page_free((void *)(uintptr_t)stack_phys); goto fail; }
    page_vas[page_count] = USER_STACK_PAGE;
    page_phys[page_count] = stack_phys;
    page_flags[page_count] = 0x3u;
    ++page_count;

    uint32_t pd = paging_create_address_space();
    if (!pd) goto fail;
    for (uint32_t i = 0; i < page_count; ++i) {
        if (paging_map_user_page_in(pd, page_vas[i], page_phys[i], page_flags[i]) != 0) {
            paging_destroy_address_space(pd);
            goto fail;
        }
    }

    struct process *process = process_create_ex(entry, stack_top, pd, page_phys, page_count);
    if (!process) {
        paging_destroy_address_space(pd);
        goto fail;
    }
    return (int)process->pid;

fail:
    for (uint32_t i = 0; i < page_count; ++i)
        if (page_phys[i]) page_free((void *)(uintptr_t)page_phys[i]);
    return -1;
}
