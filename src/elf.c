#include <stdint.h>
#include "elf.h"

#define ELF32_USER_BASE 0x10000000u
#define ELF32_USER_LIMIT 0xC0000000u
#define ELF32_PF_X 0x1u
#define ELF32_PF_MASK 0x7u

static int range_ok(uint32_t offset, uint32_t length, uint32_t size) {
    return offset <= size && length <= size - offset;
}

int elf32_validate(const uint8_t *image, uint32_t size, uint32_t *entry) {
    if (!image || size < sizeof(struct elf32_header) || !entry) return -1;

    const struct elf32_header *h = (const struct elf32_header *)image;
    if (h->ident[0] != 0x7F || h->ident[1] != 'E' ||
        h->ident[2] != 'L' || h->ident[3] != 'F') return -1;
    if (h->ident[4] != ELFCLASS32 || h->ident[5] != ELFDATA2LSB) return -1;
    if (h->type != 2u || h->machine != EM_386 || h->version != 1u) return -1;
    if (h->ehsize < sizeof(struct elf32_header) || h->ehsize > size ||
        h->phentsize < sizeof(struct elf32_phdr) || h->phnum == 0u) return -1;

    if (h->phoff > size || h->phentsize > size - h->phoff ||
        h->phnum > (size - h->phoff) / h->phentsize) return -1;

    uint32_t load_count = 0u;
    int entry_is_executable = 0;

    for (uint32_t i = 0u; i < h->phnum; ++i) {
        const uint8_t *raw = image + h->phoff + i * h->phentsize;
        const struct elf32_phdr *p = (const struct elf32_phdr *)raw;
        if (p->type != PT_LOAD) continue;
        if (p->memsz < p->filesz ||
            !range_ok(p->offset, p->filesz, size)) return -1;
        if (p->memsz == 0u) continue;
        if (p->vaddr < ELF32_USER_BASE ||
            p->vaddr >= ELF32_USER_LIMIT ||
            p->memsz > ELF32_USER_LIMIT - p->vaddr) return -1;
        if ((p->flags & ~ELF32_PF_MASK) != 0u ||
            (p->flags & ELF32_PF_MASK) == 0u) return -1;

        if (p->align > 1u) {
            if ((p->align & (p->align - 1u)) != 0u) return -1;
            if ((p->vaddr & (p->align - 1u)) !=
                (p->offset & (p->align - 1u))) return -1;
        }

        ++load_count;
        uint32_t segment_end = p->vaddr + p->memsz;
        if ((p->flags & ELF32_PF_X) != 0u &&
            h->entry >= p->vaddr && h->entry < segment_end) {
            entry_is_executable = 1;
        }
    }

    if (load_count == 0u || !entry_is_executable) return -1;
    *entry = h->entry;
    return 0;
}
