#include <stdint.h>
#include "gdt.h"
#include "tss.h"

struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

extern uint8_t stack_top;

static struct tss_entry tss;

void tss_init(void) {
    tss = (struct tss_entry){0};
    tss.esp0 = (uint32_t)(uintptr_t)&stack_top;
    tss.ss0 = 0x10u;
    tss.cs = 0x08u;
    tss.ss = 0x10u;
    tss.ds = 0x10u;
    tss.es = 0x10u;
    tss.fs = 0x10u;
    tss.gs = 0x10u;
    tss.iomap_base = sizeof(tss);

    gdt_set_tss((uint32_t)(uintptr_t)&tss, sizeof(tss) - 1u);
    __asm__ volatile ("ltr %%ax" : : "a"((uint16_t)0x18u) : "memory");
}
