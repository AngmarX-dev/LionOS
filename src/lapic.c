#include <stdint.h>
#include "lapic.h"
#include "paging.h"

#define IA32_APIC_BASE_MSR 0x1Bu
#define APIC_BASE_ENABLE 0x800u
#define CPUID_APIC_BIT (1u << 9)
#define LAPIC_REG_ID 0x020u
#define LAPIC_REG_EOI 0x0B0u
#define LAPIC_REG_SIVR 0x0F0u
#define LAPIC_REG_LVT_TIMER 0x320u
#define LAPIC_REG_TIMER_INIT 0x380u
#define LAPIC_REG_TIMER_DIV 0x3E0u
#define LAPIC_SIVR_ENABLE 0x100u
#define LAPIC_TIMER_PERIODIC (1u << 17)
#define LAPIC_TIMER_DIV_16 0x3u
#define LAPIC_TIMER_INITIAL 1000000u
#define ICR_DELIVERY_INIT (5u << 8)
#define ICR_DELIVERY_STARTUP (6u << 8)
#define ICR_LEVEL_ASSERT (1u << 14)
#define ICR_TRIGGER_LEVEL (1u << 15)
#define ICR_DELIVERY_STATUS (1u << 12)

static volatile uint32_t *lapic = (volatile uint32_t *)(uintptr_t)LIONOS_LAPIC_VIRT;
static uint32_t initialized;
static volatile uint32_t timer_ticks;

static uint64_t rdmsr(uint32_t msr) { uint32_t lo,hi; __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(msr)); return ((uint64_t)hi<<32)|lo; }
static void wrmsr(uint32_t msr,uint64_t value) { uint32_t lo=(uint32_t)value,hi=(uint32_t)(value>>32); __asm__ volatile("wrmsr"::"c"(msr),"a"(lo),"d"(hi):"memory"); }
static int cpu_has_apic(void) { uint32_t a,b,c,d; __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1u),"c"(0u)); (void)a;(void)b;(void)c; return (d&CPUID_APIC_BIT)!=0u; }
static uint32_t read_reg(uint32_t reg){return lapic[reg/4u];}
static void write_reg(uint32_t reg,uint32_t value){lapic[reg/4u]=value;}
static void wait_icr(void){for(uint32_t i=0;i<1000000u;++i){if((read_reg(0x300u)&ICR_DELIVERY_STATUS)==0u)return;__asm__ volatile("pause");}}

void lapic_enable(void){uint64_t base=rdmsr(IA32_APIC_BASE_MSR);base|=APIC_BASE_ENABLE;wrmsr(IA32_APIC_BASE_MSR,base);write_reg(LAPIC_REG_SIVR,read_reg(LAPIC_REG_SIVR)|LAPIC_SIVR_ENABLE|0xFFu);initialized=1u;}
int lapic_init(void){if(!cpu_has_apic())return-1;uint64_t base=rdmsr(IA32_APIC_BASE_MSR);if((base&APIC_BASE_ENABLE)==0u){base|=APIC_BASE_ENABLE;wrmsr(IA32_APIC_BASE_MSR,base);}if(paging_map_kernel_page(LIONOS_LAPIC_VIRT,LIONOS_LAPIC_PHYS,0x3u)!=0)return-1;lapic_enable();return 0;}
uint32_t lapic_id(void){return initialized?(read_reg(LAPIC_REG_ID)>>24):0xFFFFFFFFu;}
void lapic_eoi(void){if(initialized)write_reg(LAPIC_REG_EOI,0u);}
void lapic_send_init(uint32_t apic_id){if(!initialized)return;write_reg(0x310u,(apic_id&0xFFu)<<24);write_reg(0x300u,ICR_DELIVERY_INIT|ICR_LEVEL_ASSERT|ICR_TRIGGER_LEVEL);wait_icr();}
void lapic_send_startup(uint32_t apic_id,uint32_t vector){if(!initialized||vector>0xFFu)return;write_reg(0x310u,(apic_id&0xFFu)<<24);write_reg(0x300u,ICR_DELIVERY_STARTUP|(vector&0xFFu));wait_icr();}
void lapic_timer_init(void){if(!initialized)return;timer_ticks=0u;write_reg(LAPIC_REG_TIMER_DIV,LAPIC_TIMER_DIV_16);write_reg(LAPIC_REG_LVT_TIMER,LAPIC_TIMER_PERIODIC|LIONOS_LAPIC_TIMER_VECTOR);write_reg(LAPIC_REG_TIMER_INIT,LAPIC_TIMER_INITIAL);}
uint32_t lapic_timer_ticks(void){return timer_ticks;}
void lapic_timer_tick(void){++timer_ticks;lapic_eoi();}
