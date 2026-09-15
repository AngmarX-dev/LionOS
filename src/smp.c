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
static volatile uint32_t ap_online_mask;

static void delay(uint32_t loops){for(volatile uint32_t i=0;i<loops;++i)__asm__ volatile("pause");}
static int wait_for_online(uint32_t index){
    uint32_t mask=1u<<index;
    for(uint32_t i=0;i<LIONOS_SMP_START_TIMEOUT;++i){
        if(__atomic_load_n(&ap_online_mask,__ATOMIC_ACQUIRE)&mask)return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

uint32_t smp_lock_selftest(void){uint32_t flags=spinlock_irqsave_acquire(&smp_lock);uint32_t ok=smp_lock.value==1u;spinlock_irqrestore_release(&smp_lock,flags);return ok;}

void smp_ap_main(void){
    uint32_t index=smp_trampoline_cpu;
    debug_write("LIONOS:SMP-AP-MAIN-ENTER\n");
    if(index>=LIONOS_MAX_CPUS){debug_write("LIONOS:SMP-AP-BAD-INDEX\n");for(;;)__asm__ volatile("cli; hlt");}
    uint32_t stack_top=ap_stacks[index]+LIONOS_SMP_STACK_PAGES*4096u;
    debug_write("LIONOS:SMP-AP-TSS-BEGIN\n");
    tss_init_cpu(index,stack_top);
    debug_write("LIONOS:SMP-AP-TSS-OK\n");
    idt_load_current();
    debug_write("LIONOS:SMP-AP-IDT-OK\n");
    lapic_timer_init();
    debug_write("LIONOS:SMP-AP-TIMER-OK\n");
    __asm__ volatile("sti");
    debug_write("LIONOS:SMP-AP-STI-OK\n");
    cpu_mark_online(index,lapic_id());
    __atomic_fetch_or(&ap_online_mask,1u<<index,__ATOMIC_RELEASE);
    debug_write("LIONOS:SMP-AP-ONLINE-MARKED\n");
    for(;;)__asm__ volatile("hlt");
}

void smp_init(void){
    online_count=1u;
    ap_online_mask=0u;
    spinlock_init(&smp_lock);
    debug_write("LIONOS:SMP-ENTER\n");
    if(cpu_count_hint()<=1u){debug_write("LIONOS:SMP-CPU-COUNT-1\n");return;}
    uint32_t bsp_id=lapic_id();
    if(bsp_id==0xFFFFFFFFu){debug_write("LIONOS:SMP-NO-BSP-APIC\n");return;}
    debug_write("LIONOS:SMP-BSP-APIC-OK\n");
    smp_trampoline_cr3=paging_kernel_directory();
    smp_trampoline_entry=(uint32_t)(uintptr_t)&smp_ap_main;
    uint32_t target_count=cpu_count_hint();
    if(target_count>LIONOS_MAX_CPUS)target_count=LIONOS_MAX_CPUS;
    for(uint32_t index=1u;index<target_count;++index){
        uint32_t stack_pages=LIONOS_SMP_STACK_PAGES;
        ap_stacks[index]=(uint32_t)(uintptr_t)page_alloc_contiguous(stack_pages);
        if(!ap_stacks[index]){debug_write("LIONOS:SMP-STACK-FAIL\n");break;}
        debug_write("LIONOS:SMP-STACK-OK\n");
        smp_trampoline_stack=ap_stacks[index]+stack_pages*4096u;
        smp_trampoline_cpu=index;
        uint32_t target_apic=bsp_id+index;
        debug_write("LIONOS:SMP-INIT\n");
        lapic_send_init(target_apic);
        debug_write("LIONOS:SMP-INIT-DONE\n");
        delay(10000000u);
        debug_write("LIONOS:SMP-SIPI1\n");
        lapic_send_startup(target_apic,LIONOS_SMP_TRAMPOLINE>>12);
        debug_write("LIONOS:SMP-SIPI1-DONE\n");
        if(wait_for_online(index)!=0){
            debug_write("LIONOS:SMP-SIPI1-TIMEOUT\n");
            debug_write("LIONOS:SMP-SIPI2\n");
            lapic_send_startup(target_apic,LIONOS_SMP_TRAMPOLINE>>12);
            debug_write("LIONOS:SMP-SIPI2-DONE\n");
        }
        if(wait_for_online(index)==0){
            ++online_count;
            console_write("[ OK ] CPU ");
            console_write_dec(index);
            console_write(" online / APIC ");
            console_write_dec(target_apic);
            console_write(" / per-CPU TSS ready\n");
            debug_write("LIONOS:SMP-CPU-ONLINE\n");
        }else{
            console_write("[ -- ] CPU ");
            console_write_dec(index);
            console_write(" AP startup timeout\n");
            debug_write("LIONOS:SMP-AP-TIMEOUT\n");
            break;
        }
    }
}
uint32_t smp_online_count(void){return online_count;}
