#include <stdint.h>
#include "elf.h"
#include "exec.h"
#include "ramfs.h"

#define USER_LIMIT 0xC0000000u
#define USER_BASE 0x00400000u
#define USER_STACK_TOP 0xBFFFF000u
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
        if (p->vaddr < USER_BASE) return -1;
        if (p->vaddr >= USER_LIMIT || p->memsz > USER_LIMIT - p->vaddr) return -1;
        if (align_down(p->vaddr) < USER_BASE) return -1;
        if ((p->flags & 0x7u) == 0) return -1;
    }
    if (!load_count || *entry < USER_BASE || *entry >= USER_LIMIT) return -1;
    *stack_top = USER_STACK_TOP;
    return 0;
}

int exec_run_file(const char *name) {
    const char *image = ramfs_data(name);
    uint32_t size = ramfs_size(name);
    uint32_t entry, stack_top;
    if (!image || !size || exec_validate_image((const uint8_t *)image, size, &entry, &stack_top) != 0)
        return -1;
    (void)entry;
    (void)stack_top;
    return 0;
}
