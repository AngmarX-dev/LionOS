#ifndef LIONOS_ELF_H
#define LIONOS_ELF_H

#include <stdint.h>

#define ELF32_EI_NIDENT 16u
#define ELFCLASS32 1u
#define ELFDATA2LSB 1u
#define EM_386 3u
#define PT_LOAD 1u

struct elf32_header {
    uint8_t ident[ELF32_EI_NIDENT];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed));

struct elf32_phdr {
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
} __attribute__((packed));

int elf32_validate(const uint8_t *image, uint32_t size, uint32_t *entry);

#endif
