#include <stdint.h>
#include "gdt.h"
#include "tss.h"
#include "cpu.h"

struct tss_entry { uint32_t prev_tss; uint32_t esp0; uint32_t ss0; uint32_t esp1; uint32_t ss1; uint32_t esp2; uint32_t ss2; uint32_t cr3; uint32_t eip; uint32_t eflags; uint32_t eax; uint32_t ecx; uint32_t edx; uint32_t ebx; uint32_t esp; uint32_t ebp; uint32_t esi; uint32_t edi; uint32_t es; uint32_t cs; uint32_t ss; uint32_t ds; uint32_t fs; uint32_t gs; uint32_t ldt; uint16_t trap; uint16_t iomap_base; } __attribute__((packed));

extern uint8_t stack_top;
static struct tss_entry tsses[LIONOS_MAX_CPUS];

void tss_init_cpu(uint32_t cpu,uint32_t stack_top_value){
    if(cpu>=LIONOS_MAX_CPUS) return;
    struct tss_entry *t=&tsses[cpu];
    *t=(struct tss_entry){0};
    t->esp0=stack_top_value;
    t->ss0=0x10u; t->cs=0x08u; t->ss=0x10u; t->ds=0x10u; t->es=0x10u; t->fs=0x10u; t->gs=0x10u;
    t->iomap_base=(uint16_t)sizeof(*t);
    gdt_load_cpu(cpu,(uint32_t)(uintptr_t)t,(uint32_t)sizeof(*t)-1u);
    __asm__ volatile("ltr %%ax" : : "a"((uint16_t)gdt_tss_selector()) : "memory");
}

void tss_init(void){tss_init_cpu(0u,(uint32_t)(uintptr_t)&stack_top);}

void tss_set_kernel_stack(uint32_t stack_top_value){
    uint32_t cpu=cpu_current_index();
    if(cpu<LIONOS_MAX_CPUS&&stack_top_value)tsses[cpu].esp0=stack_top_value;
}
