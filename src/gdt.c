#include <stdint.h>
#include "gdt.h"

#define LIONOS_MAX_CPUS 16u
#define GDT_ENTRIES 7u
#define GDT_TSS 0x18u
#define GDT_USER_CODE 0x2Bu
#define GDT_USER_DATA 0x33u

struct gdt_entry { uint16_t limit_low; uint16_t base_low; uint8_t base_mid; uint8_t access; uint8_t granularity; uint8_t base_high; } __attribute__((packed));
struct gdt_ptr { uint16_t limit; uint32_t base; } __attribute__((packed));

static struct gdt_entry gdts[LIONOS_MAX_CPUS][GDT_ENTRIES];
static struct gdt_ptr gps[LIONOS_MAX_CPUS];

static void set_gate(struct gdt_entry *gdt, uint32_t n, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[n].base_low=(uint16_t)(base&0xFFFFu); gdt[n].base_mid=(uint8_t)((base>>16)&0xFFu); gdt[n].base_high=(uint8_t)((base>>24)&0xFFu);
    gdt[n].limit_low=(uint16_t)(limit&0xFFFFu); gdt[n].granularity=(uint8_t)(((limit>>16)&0x0Fu)|(gran&0xF0u)); gdt[n].access=access;
}

static void set_tss_gate(struct gdt_entry *gdt,uint32_t base,uint32_t limit){
    set_gate(gdt,3u,base,limit,0x89u,0x00u);
    set_gate(gdt,4u,0u,0u,0u,0u);
}

static void load_cpu_gdt(uint32_t cpu,uint32_t tss_base,uint32_t tss_limit){
    if(cpu>=LIONOS_MAX_CPUS)cpu=0u;
    struct gdt_entry *gdt=gdts[cpu];
    for(uint32_t i=0;i<GDT_ENTRIES;++i)gdt[i]=(struct gdt_entry){0};
    set_gate(gdt,1u,0u,0xFFFFFFFFu,0x9Au,0xCFu);
    set_gate(gdt,2u,0u,0xFFFFFFFFu,0x92u,0xCFu);
    set_tss_gate(gdt,tss_base,tss_limit);
    set_gate(gdt,5u,0u,0xFFFFFFFFu,0xFAu,0xCFu);
    set_gate(gdt,6u,0u,0xFFFFFFFFu,0xF2u,0xCFu);
    gps[cpu].limit=(uint16_t)(sizeof(gdts[cpu])-1u); gps[cpu].base=(uint32_t)(uintptr_t)gdts[cpu];
    __asm__ volatile("lgdt %0\nmov $0x10, %%ax\nmov %%ax, %%ds\nmov %%ax, %%es\nmov %%ax, %%fs\nmov %%ax, %%gs\nmov %%ax, %%ss\nljmp $0x08, $1f\n1:\n" : : "m"(gps[cpu]) : "ax","memory");
}

void gdt_init(void){load_cpu_gdt(0u,0u,0u);}
void gdt_load_cpu(uint32_t cpu,uint32_t tss_base,uint32_t tss_limit){load_cpu_gdt(cpu,tss_base,tss_limit);}
void gdt_set_tss(uint32_t base,uint32_t limit){load_cpu_gdt(0u,base,limit);}
uint32_t gdt_user_code_selector(void){return GDT_USER_CODE;}
uint32_t gdt_user_data_selector(void){return GDT_USER_DATA;}
uint32_t gdt_tss_selector(void){return GDT_TSS;}
