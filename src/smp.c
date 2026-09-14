#include <stdint.h>
#include "smp.h"
#include "cpu.h"
#include "lapic.h"
#include "memory.h"
#include "paging.h"
#include "console.h"
#include "debug.h"
#include "tss.h"
#include "idt.h"
#include "spinlock.h"

extern uint32_t smp_trampoline_cr3;
extern uint32_t smp_trampoline_entry;
extern uint32_t smp_trampoline_stack;
extern uint32_t smp_trampoline_cpu;

static uint32_t online_count=1u;
static uint32_t ap_stacks[LIONOS_MAX_CPUS];
static struct spinlock smp_lock;
static void delay(uint32_t loops){for(volatile uint32_t i=0;i<loops;++i)__asm__ volatile("pause");}
static int wait_for_online(uint32_t index){for(uint32_t i=0;i<LIONOS_SMP_START_TIMEOUT;++i){const struct cpu_info*c=cpu_get(index);if(c&&c->online)return 0;__asm__ volatile("pause");}return-1;}

uint32_t smp_lock_selftest(void){uint32_t flags=spinlock_irqsave_acquire(&smp_lock);uint32_t ok=smp_lock.value==1u;spinlock_irqrestore_release(&smp_lock,flags);return ok;}

void smp_ap_main(void){
    uint32_t index=smp_trampoline_cpu;
    if(index>=LIONOS_MAX_CPUS)for(;;)__asm__ volatile("cli; hlt");
    uint32_t stack_top=ap_stacks[index]+LIONOS_SMP_STACK_PAGES*4096u;
    cpu_mark_online(index,lapic_id());
    tss_init_cpu(index,stack_top);
    /* The AP arrives from reset with an unusable/temporary IDT.  Load the
       kernel IDT before enabling interrupts so every AP uses the same ISR
       table, including the LAPIC timer vector. */
    idt_load_current();
    /* Each CPU owns its local LAPIC timer.  Starting it here lets the AP
       enter the same timer-driven scheduler path as the BSP. */
    lapic_timer_init();
    __asm__ volatile("sti");
    for(;;)__asm__ volatile("hlt");
}

void smp_init(void){
    online_count=1u;
    spinlock_init(&smp_lock);
    if(cpu_count_hint()<=1u||lapic_id()==0xFFFFFFFFu)return;
    smp_trampoline_cr3=paging_kernel_directory();
    smp_trampoline_entry=(uint32_t)(uintptr_t)&smp_ap_main;
    uint32_t bsp_id=lapic_id(),target_count=cpu_count_hint();
    if(target_count>LIONOS_MAX_CPUS)target_count=LIONOS_MAX_CPUS;
    for(uint32_t index=1u;index<target_count;++index){
        uint32_t stack_pages=LIONOS_SMP_STACK_PAGES;
        ap_stacks[index]=(uint32_t)(uintptr_t)page_alloc_contiguous(stack_pages);
        if(!ap_stacks[index])break;
        smp_trampoline_stack=ap_stacks[index]+stack_pages*4096u;
        smp_trampoline_cpu=index;
        uint32_t target_apic=bsp_id+index;
        lapic_send_init(target_apic);delay(100000u);
        lapic_send_startup(target_apic,LIONOS_SMP_TRAMPOLINE>>12);delay(200000u);
        lapic_send_startup(target_apic,LIONOS_SMP_TRAMPOLINE>>12);
        if(wait_for_online(index)==0){++online_count;console_write("[ OK ] CPU ");console_write_dec(index);console_write(" online / APIC ");console_write_dec(target_apic);console_write(" / per-CPU TSS ready\n");debug_write("LIONOS:SMP-CPU-ONLINE\n");}
        else{console_write("[ -- ] CPU ");console_write_dec(index);console_write(" AP startup timeout\n");debug_write("LIONOS:SMP-CPU-TIMEOUT\n");break;}
    }
}
uint32_t smp_online_count(void){return online_count;}
