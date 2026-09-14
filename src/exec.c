#include <stdint.h>
#include "elf.h"
#include "exec.h"
#include "memory.h"
#include "paging.h"
#include "process.h"
#include "ramfs.h"

#define USER_BASE 0x00400000u
#define USER_STACK_VA 0x00401000u
#define USER_STACK_TOP 0x00402000u
#define PAGE_SIZE 4096u

static uint32_t align_down(uint32_t x) { return x & ~(PAGE_SIZE - 1u); }

int exec_validate_image(const uint8_t *image, uint32_t size, uint32_t *entry, uint32_t *stack_top) {
    if (elf32_validate(image, size, entry) != 0) return -1;
    const struct elf32_header *h = (const struct elf32_header *)image;
    uint32_t load_count = 0;
    for (uint32_t i = 0; i < h->phnum; ++i) {
        const struct elf32_phdr *p = (const struct elf32_phdr *)(image + h->phoff + i * h->phentsize);
        if (p->type != PT_LOAD || p->memsz == 0) continue;
        ++load_count;
        if (p->vaddr < USER_BASE || p->vaddr >= 0xC0000000u) return -1;
        if (p->memsz > 0xC0000000u - p->vaddr) return -1;
        if (align_down(p->vaddr) != USER_BASE || p->memsz > PAGE_SIZE) return -1;
        if ((p->flags & 0x7u) == 0) return -1;
    }
    if (!load_count || *entry < USER_BASE || *entry >= 0xC0000000u) return -1;
    *stack_top = USER_STACK_TOP;
    return 0;
}

int exec_run_file(const char *name) {
    const char *image = ramfs_data(name);
    uint32_t size = ramfs_size(name);
    uint32_t entry, stack_top;
    if (!image || !size || exec_validate_image((const uint8_t *)image, size, &entry, &stack_top) != 0) return -1;

    const struct elf32_header *h = (const struct elf32_header *)image;
    const struct elf32_phdr *load = 0;
    for (uint32_t i = 0; i < h->phnum; ++i) {
        const struct elf32_phdr *p = (const struct elf32_phdr *)(image + h->phoff + i * h->phentsize);
        if (p->type == PT_LOAD && p->memsz) { load = p; break; }
    }
    if (!load) return -1;

    uint8_t *code = (uint8_t *)page_alloc();
    uint8_t *stack = (uint8_t *)page_alloc();
    if (!code || !stack) { if (code) page_free(code); if (stack) page_free(stack); return -1; }
    for (uint32_t i = 0; i < PAGE_SIZE; ++i) { code[i] = 0x90u; stack[i] = 0; }
    for (uint32_t i = 0; i < load->filesz; ++i) code[(load->vaddr - USER_BASE) + i] = ((const uint8_t *)image)[load->offset + i];

    uint32_t pd = paging_create_address_space();
    if (!pd || paging_map_user_page_in(pd, USER_BASE, (uint32_t)code, (load->flags & 2u) ? 0x7u : 0x5u) != 0 ||
        paging_map_user_page_in(pd, USER_STACK_VA, (uint32_t)stack, 0x7u) != 0) {
        if (pd) paging_destroy_address_space(pd);
        page_free(code); page_free(stack); return -1;
    }

    struct process *p = process_create(entry, stack_top, pd, (uint32_t)code, (uint32_t)stack);
    if (!p) { paging_destroy_address_space(pd); page_free(code); page_free(stack); return -1; }
    return (int)p->pid;
}
