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
extern volatile uint32_t smp_trampoline_cr0_cache;

static uint32_t online_count=1u;
static uint32_t ap_stacks[LIONOS_MAX_CPUS];
static struct spinlock smp_lock;
static volatile uint32_t ap_online_mask;

/* Early-boot SMP lock contention test, serviced by AP idle loops. */
#define SMP_STRESS_ITERATIONS 4096u
#define SMP_STRESS_TIMEOUT 100000000u
static struct spinlock smp_stress_lock;
static volatile uint32_t smp_stress_request;
static volatile uint32_t smp_stress_ready_mask;
static volatile uint32_t smp_stress_go;
static volatile uint32_t smp_stress_done_mask;
static uint32_t smp_stress_counter;
static uint32_t smp_stress_active;
static uint32_t smp_stress_violations;

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

static void smp_lock_stress_worker(uint32_t cpu_index){
    if(cpu_index>=LIONOS_MAX_CPUS)return;
    uint32_t cpu_bit=1u<<cpu_index;
    if(__atomic_load_n(&smp_stress_done_mask,__ATOMIC_ACQUIRE)&cpu_bit)return;

    if(cpu_index!=0u){
        __atomic_fetch_or(&smp_stress_ready_mask,cpu_bit,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&smp_stress_go,__ATOMIC_ACQUIRE)){
            if(!__atomic_load_n(&smp_stress_request,__ATOMIC_ACQUIRE))return;
            __asm__ volatile("pause");
        }
    }

    for(uint32_t i=0u;i<SMP_STRESS_ITERATIONS;++i){
        spinlock_acquire(&smp_stress_lock);
        uint32_t active=__atomic_add_fetch(&smp_stress_active,1u,__ATOMIC_SEQ_CST);
        if(active!=1u)
            (void)__atomic_add_fetch(&smp_stress_violations,1u,__ATOMIC_RELAXED);
        (void)__atomic_add_fetch(&smp_stress_counter,1u,__ATOMIC_RELAXED);
        (void)__atomic_sub_fetch(&smp_stress_active,1u,__ATOMIC_SEQ_CST);
        spinlock_release(&smp_stress_lock);
    }

    __atomic_fetch_or(&smp_stress_done_mask,cpu_bit,__ATOMIC_RELEASE);
}

static void smp_lock_stress_selftest(void){
    if(online_count==0u||online_count>LIONOS_MAX_CPUS){
        debug_write("LIONOS:SMP-LOCK-STRESS-FAIL-CPU-COUNT\n");
        return;
    }

    spinlock_init(&smp_stress_lock);
    smp_stress_counter=0u;
    smp_stress_active=0u;
    smp_stress_violations=0u;
    __atomic_store_n(&smp_stress_ready_mask,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&smp_stress_go,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&smp_stress_done_mask,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&smp_stress_request,1u,__ATOMIC_RELEASE);

    uint32_t expected_mask=(1u<<online_count)-1u;
    uint32_t expected_ap_mask=expected_mask&~1u;
    uint32_t ready=0u;
    for(uint32_t i=0u;i<SMP_STRESS_TIMEOUT;++i){
        ready=__atomic_load_n(&smp_stress_ready_mask,__ATOMIC_ACQUIRE);
        if((ready&expected_ap_mask)==expected_ap_mask)break;
        __asm__ volatile("pause");
    }
    if((ready&expected_ap_mask)!=expected_ap_mask){
        __atomic_store_n(&smp_stress_request,0u,__ATOMIC_RELEASE);
        debug_write("LIONOS:SMP-LOCK-STRESS-FAIL-READY-TIMEOUT\n");
        return;
    }

    /* Release all AP workers together, then contend with them on the BSP. */
    __atomic_store_n(&smp_stress_go,1u,__ATOMIC_RELEASE);
    smp_lock_stress_worker(0u);

    uint32_t completed=0u;
    for(uint32_t i=0u;i<SMP_STRESS_TIMEOUT;++i){
        completed=__atomic_load_n(&smp_stress_done_mask,__ATOMIC_ACQUIRE);
        if((completed&expected_mask)==expected_mask)break;
        __asm__ volatile("pause");
    }
    __atomic_store_n(&smp_stress_request,0u,__ATOMIC_RELEASE);

    uint32_t expected_count=online_count*SMP_STRESS_ITERATIONS;
    uint32_t actual_count=__atomic_load_n(&smp_stress_counter,__ATOMIC_RELAXED);
    uint32_t active=__atomic_load_n(&smp_stress_active,__ATOMIC_SEQ_CST);
    uint32_t violations=__atomic_load_n(&smp_stress_violations,__ATOMIC_RELAXED);
    if((completed&expected_mask)!=expected_mask||actual_count!=expected_count||
       active!=0u||violations!=0u){
        debug_write("LIONOS:SMP-LOCK-STRESS-FAIL\n");
        return;
    }

    debug_write("LIONOS:SMP-LOCK-STRESS-OK\n");
}

void smp_ap_main(void){
    uint32_t index=smp_trampoline_cpu;
    debug_write("LIONOS:SMP-AP-MAIN-ENTER\n");
    if(index>=LIONOS_MAX_CPUS){debug_write("LIONOS:SMP-AP-BAD-INDEX\n");for(;;)__asm__ volatile("cli; hlt");}
    uint32_t stack_top=ap_stacks[index]+LIONOS_SMP_STACK_PAGES*4096u;
    paging_switch_address_space(paging_kernel_directory());
    int memory_type_status = cpu_memory_type_apply_current(index);
    if(memory_type_status!=0){
        debug_write("LIONOS:SMP-AP-MEMTYPE-APPLY-FAIL\n");
        for(;;)__asm__ volatile("cli; hlt");
    }
    debug_write("LIONOS:SMP-AP-MTRR-SYNC-DONE\n");
    memory_type_status = cpu_memory_type_validate_current(index);
    if (memory_type_status != 0) {
        static const char *const mismatch_markers[] = {
            0,
            "LIONOS:SMP-AP-MEMTYPE-INVALID-INDEX\n",
            "LIONOS:SMP-AP-MEMTYPE-APIC-ID-MISMATCH\n",
            "LIONOS:SMP-AP-MEMTYPE-FEATURE-MISMATCH\n",
            "LIONOS:SMP-AP-MEMTYPE-PAT-MISMATCH\n",
            "LIONOS:SMP-AP-MEMTYPE-CR0-MISMATCH\n",
            "LIONOS:SMP-AP-MEMTYPE-MTRR-HEADER-MISMATCH\n",
            "LIONOS:SMP-AP-MEMTYPE-MTRR-SNAPSHOT-INVALID\n",
            "LIONOS:SMP-AP-MEMTYPE-FIXED-MTRR-MISMATCH\n",
            "LIONOS:SMP-AP-MEMTYPE-VARIABLE-MTRR-MISMATCH\n",
            "LIONOS:SMP-AP-MEMTYPE-PHYS-WIDTH-UNSUPPORTED\n",
            "LIONOS:SMP-AP-MEMTYPE-PHYS-WIDTH-MISMATCH\n"
        };
        if ((uint32_t)memory_type_status <
            (uint32_t)(sizeof(mismatch_markers) / sizeof(mismatch_markers[0])))
            debug_write(mismatch_markers[memory_type_status]);
        debug_write("LIONOS:SMP-AP-MEMORY-TYPE-MISMATCH\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    debug_write("LIONOS:SMP-AP-MEMORY-TYPE-VERIFIED\n");
    /* Shared TSS state is touched only after this AP has matching MTRRs. */
    debug_write("LIONOS:SMP-AP-TSS-BEGIN\n");
    tss_init_cpu(index,stack_top);
    if(cpu_nx_enabled()&&cpu_enable_nx()!=0){
        debug_write("LIONOS:SMP-AP-NX-ENABLE-FAIL\n");
        for(;;)__asm__ volatile("cli; hlt");
    }
    (void)cpu_fpu_sse_init();
    debug_write("LIONOS:SMP-AP-TSS-OK\n");
    idt_load_current();
    debug_write("LIONOS:SMP-AP-IDT-OK\n");
    lapic_enable();
    lapic_timer_init();
    debug_write("LIONOS:SMP-AP-TIMER-OK\n");
    __asm__ volatile("sti");
    debug_write("LIONOS:SMP-AP-STI-OK\n");
    cpu_mark_online(index,lapic_id());
    __atomic_fetch_or(&ap_online_mask,1u<<index,__ATOMIC_RELEASE);
    debug_write("LIONOS:SMP-AP-ONLINE-MARKED\n");
    for(;;){
        if(__atomic_load_n(&smp_stress_request,__ATOMIC_ACQUIRE))
            smp_lock_stress_worker(index);
        /*
         * Keep the idle transition atomic with respect to maskable
         * interrupts. A reschedule/stress IPI arriving after the work check
         * but before HLT must remain pending and wake this AP.
         */
        __asm__ volatile("sti; hlt" ::: "memory");
    }
}

void smp_init(void){
    online_count=1u;
    ap_online_mask=0u;
    spinlock_init(&smp_lock);
    debug_write("LIONOS:SMP-ENTER\n");
    if(cpu_count_hint()<=1u){
        debug_write("LIONOS:SMP-CPU-COUNT-1\n");
        smp_lock_stress_selftest();
        return;
    }
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
        uint32_t target_apic=cpu_get(index)?cpu_get(index)->apic_id:0xFFFFFFFFu;
        if(target_apic==0xFFFFFFFFu||target_apic>0xFFu){debug_write("LIONOS:SMP-BAD-APIC-ID\n");break;}
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
    smp_lock_stress_selftest();
}
uint32_t smp_online_count(void){return online_count;}
