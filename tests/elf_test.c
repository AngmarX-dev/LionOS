#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "elf.h"

#define IMAGE_SIZE 256u
#define PH_OFFSET ((uint32_t)sizeof(struct elf32_header))

static uint8_t image[IMAGE_SIZE];

static void make_valid_image(void) {
    memset(image, 0, sizeof(image));
    struct elf32_header *h = (struct elf32_header *)image;
    h->ident[0] = 0x7f; h->ident[1] = 'E';
    h->ident[2] = 'L'; h->ident[3] = 'F';
    h->ident[4] = ELFCLASS32; h->ident[5] = ELFDATA2LSB;
    h->type = 2u; h->machine = EM_386; h->version = 1u;
    h->entry = 0x10001000u; h->phoff = PH_OFFSET;
    h->ehsize = sizeof(*h); h->phentsize = sizeof(struct elf32_phdr);
    h->phnum = 1u;

    struct elf32_phdr *p = (struct elf32_phdr *)(image + PH_OFFSET);
    p->type = PT_LOAD; p->offset = 0x100u; p->vaddr = 0x10001000u;
    p->filesz = 16u; p->memsz = 32u; p->flags = 5u; p->align = 0x1000u;
}

static int expect_valid(const char *name, int expected) {
    uint32_t entry = 0u;
    int result = elf32_validate(image, sizeof(image), &entry);
    if ((result == 0) != (expected != 0)) {
        fprintf(stderr, "ELF test failed: %s (result=%d)\n", name, result);
        return 1;
    }
    if (expected && entry != 0x10001000u) {
        fprintf(stderr, "ELF test failed: %s (entry mismatch)\n", name);
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    make_valid_image();
    failures += expect_valid("valid executable", 1);

    make_valid_image();
    ((struct elf32_header *)image)->entry = 0x10002000u;
    failures += expect_valid("entry outside load segment", 0);

    make_valid_image();
    ((struct elf32_phdr *)(image + PH_OFFSET))->flags = 6u;
    failures += expect_valid("entry in non-executable segment", 0);

    make_valid_image();
    ((struct elf32_phdr *)(image + PH_OFFSET))->align = 0x1800u;
    failures += expect_valid("non-power-of-two alignment", 0);

    make_valid_image();
    ((struct elf32_phdr *)(image + PH_OFFSET))->offset = 0x101u;
    failures += expect_valid("incongruent segment alignment", 0);

    make_valid_image();
    ((struct elf32_phdr *)(image + PH_OFFSET))->flags = 0x9u;
    failures += expect_valid("unknown segment flags", 0);

    make_valid_image();
    ((struct elf32_phdr *)(image + PH_OFFSET))->filesz = 33u;
    failures += expect_valid("file size exceeds memory size", 0);

    make_valid_image();
    ((struct elf32_header *)image)->phoff = IMAGE_SIZE - 4u;
    failures += expect_valid("truncated program header table", 0);

    make_valid_image();
    ((struct elf32_phdr *)(image + PH_OFFSET))->memsz = 0u;
    ((struct elf32_phdr *)(image + PH_OFFSET))->filesz = 1u;
    failures += expect_valid("nonempty file in empty memory segment", 0);

    make_valid_image();
    ((struct elf32_phdr *)(image + PH_OFFSET))->type = 0u;
    failures += expect_valid("no loadable segment", 0);

    if (failures) return 1;
    puts("ELF validation tests: PASS");
    return 0;
}
