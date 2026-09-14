#include <stdint.h>
#include "cpu.h"
#include "lapic.h"

static struct cpu_info cpus[LIONOS_MAX_CPUS];
static uint32_t cpu_hint=1u;

static void cpuid(uint32_t leaf,uint32_t subleaf,uint32_t*eax,uint32_t*ebx,uint32_t*ecx,uint32_t*edx){__asm__ volatile("cpuid":"=a"(*eax),"=b"(*ebx),"=c"(*ecx),"=d"(*edx):"a"(leaf),"c"(subleaf));}

void cpu_init(void){for(uint32_t i=0;i<LIONOS_MAX_CPUS;++i){cpus[i].index=i;cpus[i].apic_id=0xFFFFFFFFu;cpus[i].logical_per_package=1u;cpus[i].online=0u;}uint32_t max_leaf,ebx,ecx,edx;cpuid(0u,0u,&max_leaf,&ebx,&ecx,&edx);uint32_t eax1,ebx1,ecx1,edx1;cpuid(1u,0u,&eax1,&ebx1,&ecx1,&edx1);(void)eax1;(void)ecx1;(void)edx1;(void)ebx;(void)ecx;(void)edx;uint32_t logical=(ebx1>>16)&0xFFu;if(logical==0u)logical=1u;if(logical>LIONOS_MAX_CPUS)logical=LIONOS_MAX_CPUS;uint32_t apic=(ebx1>>24)&0xFFu;cpu_hint=logical;cpus[0].apic_id=apic;cpus[0].logical_per_package=logical;cpus[0].online=1u;if(max_leaf>=0xBu){uint32_t a,b,c,d;cpuid(0xBu,0u,&a,&b,&c,&d);if(b){uint32_t topology=b&0xFFFFu;if(topology&&topology<=LIONOS_MAX_CPUS)cpu_hint=topology;}}}
uint32_t cpu_count_hint(void){return cpu_hint;}
uint32_t cpu_current_index(void){uint32_t id=lapic_id();if(id!=0xFFFFFFFFu)for(uint32_t i=0;i<LIONOS_MAX_CPUS;++i)if(cpus[i].online&&cpus[i].apic_id==id)return i;return 0u;}
const struct cpu_info*cpu_get(uint32_t index){return index<LIONOS_MAX_CPUS?&cpus[index]:0;}
void cpu_mark_online(uint32_t index,uint32_t apic_id){if(index>=LIONOS_MAX_CPUS)return;cpus[index].apic_id=apic_id;cpus[index].logical_per_package=1u;cpus[index].online=1u;}
