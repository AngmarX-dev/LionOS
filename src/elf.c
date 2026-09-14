#include <stdint.h>
#include "elf.h"

static int range_ok(uint32_t offset, uint32_t length, uint32_t size) {
    return offset <= size && length <= size - offset;
}

int elf32_validate(const uint8_t *image, uint32_t size, uint32_t *entry) {
    if (!image || size < sizeof(struct elf32_header) || !entry) return -1;

    const struct elf32_header *h = (const struct elf32_header *)image;
    if (h->ident[0] != 0x7F || h->ident[1] != 'E' ||
        h->ident[2] != 'L' || h->ident[3] != 'F') return -1;
    if (h->ident[4] != ELFCLASS32 || h->ident[5] != ELFDATA2LSB) return -1;
    if (h->machine != EM_386 || h->version != 1u) return -1;
    if (h->ehsize < sizeof(struct elf32_header) ||
        h->phentsize < sizeof(struct elf32_phdr) || h->phnum == 0) return -1;
    if (h->phoff > size || h->phnum > (size - h->phoff) / h->phentsize) return -1;

    uint32_t load_count = 0;
    for (uint32_t i = 0; i < h->phnum; ++i) {
        const uint8_t *raw = image + h->phoff + i * h->phentsize;
        const struct elf32_phdr *p = (const struct elf32_phdr *)raw;
        if (p->type != PT_LOAD) continue;
        ++load_count;
        if (p->memsz < p->filesz || !range_ok(p->offset, p->filesz, size)) return -1;
        if (p->vaddr < 0x00400000u) return -1;
        if (p->vaddr >= 0xC0000000u || p->memsz > 0xC0000000u - p->vaddr) return -1;
        if ((p->flags & 0x7u) == 0) return -1;
    }

    if (!load_count || h->entry < 0x00400000u || h->entry >= 0xC0000000u) return -1;
    *entry = h->entry;
    return 0;
}
