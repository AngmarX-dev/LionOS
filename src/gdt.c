#include <stdint.h>
#include "gdt.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct gdt_entry gdt[4];
static struct gdt_ptr gp;

static void set_gate(int n, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[n].base_low = base & 0xFFFFu;
    gdt[n].base_mid = (base >> 16) & 0xFFu;
    gdt[n].base_high = (base >> 24) & 0xFFu;
    gdt[n].limit_low = limit & 0xFFFFu;
    gdt[n].granularity = ((limit >> 16) & 0x0Fu) | (gran & 0xF0u);
    gdt[n].access = access;
}

void gdt_set_tss(uint32_t base, uint32_t limit) {
    set_gate(3, base, limit, 0x89, 0x00);
}

void gdt_init(void) {
    gp.limit = sizeof(gdt) - 1u;
    gp.base = (uint32_t)&gdt;

    set_gate(0, 0, 0, 0, 0);
    set_gate(1, 0, 0xFFFFFFFFu, 0x9Au, 0xCFu);
    set_gate(2, 0, 0xFFFFFFFFu, 0x92u, 0xCFu);
    set_gate(3, 0, 0, 0, 0);

    __asm__ volatile ("lgdt %0\n"
                      "mov $0x10, %%ax\n"
                      "mov %%ax, %%ds\n"
                      "mov %%ax, %%es\n"
                      "mov %%ax, %%fs\n"
                      "mov %%ax, %%gs\n"
                      "mov %%ax, %%ss\n"
                      "ljmp $0x08, $1f\n"
                      "1:\n"
                      : : "m"(gp) : "ax", "memory");
}
