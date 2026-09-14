#include <stdint.h>
#include "elf.h"
#include "exec.h"
#include "ramfs.h"

#define USER_LIMIT 0xC0000000u
#define USER_STACK_TOP 0xBFFFF000u
#define PAGE_SIZE 4096u

static uint32_t align_down(uint32_t x) { return x & ~(PAGE_SIZE - 1u); }
static uint32_t align_up(uint32_t x) {
    if (x > USER_LIMIT - (PAGE_SIZE - 1u)) return USER_LIMIT;
    return (x + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
}

int exec_validate_image(const uint8_t *image, uint32_t size, uint32_t *entry, uint32_t *stack_top) {
    if (elf32_validate(image, size, entry) != 0) return -1;
    const struct elf32_header *h = (const struct elf32_header *)image;
    uint32_t highest = 0x00400000u;
    for (uint32_t i = 0; i < h->phnum; ++i) {
        const struct elf32_phdr *p = (const struct elf32_phdr *)(image + h->phoff + i * h->phentsize);
        if (p->type != PT_LOAD || p->memsz == 0) continue;
        uint32_t end = p->vaddr + p->memsz;
        if (end < p->vaddr || end >= USER_LIMIT) return -1;
        if (end > highest) highest = end;
        if (p->vaddr < PAGE_SIZE) return -1;
        if (align_down(p->vaddr) < 0x00400000u) return -1;
    }
    if (*entry < 0x00400000u || *entry >= USER_LIMIT) return -1;
    *stack_top = USER_STACK_TOP;
    (void)highest;
    return 0;
}

int exec_run_file(const char *name) {
    const char *image = ramfs_data(name);
    uint32_t size = ramfs_size(name);
    uint32_t entry, stack_top;
    if (!image || !size || exec_validate_image((const uint8_t *)image, size, &entry, &stack_top) != 0)
        return -1;
    return 0;
}
