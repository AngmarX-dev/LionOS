#include <stdint.h>
#include "smp.h"
#include "cpu.h"
#include "lapic.h"
#include "memory.h"
#include "paging.h"
#include "console.h"
#include "debug.h"

extern uint32_t smp_trampoline_cr3;
extern uint32_t smp_trampoline_entry;
extern uint32_t smp_trampoline_stack;
extern uint32_t smp_trampoline_cpu;

static uint32_t online_count = 1u;
static uint32_t ap_stacks[LIONOS_MAX_CPUS];

static void delay(uint32_t loops) {
    for (volatile uint32_t i = 0; i < loops; ++i)
        __asm__ volatile("pause");
}

static int wait_for_online(uint32_t index) {
    for (uint32_t i = 0; i < LIONOS_SMP_START_TIMEOUT; ++i) {
        const struct cpu_info *cpu = cpu_get(index);
        if (cpu && cpu->online) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

void smp_ap_main(void) {
    uint32_t index = smp_trampoline_cpu;
    if (index >= LIONOS_MAX_CPUS) {
        for (;;) __asm__ volatile("cli; hlt");
    }

    cpu_mark_online(index, lapic_id());
    /* Keep the AP quiescent until per-CPU IDT/TSS and scheduler state exist. */
    for (;;) __asm__ volatile("cli; hlt");
}

void smp_init(void) {
    online_count = 1u;
    if (cpu_count_hint() <= 1u) return;
    if (lapic_id() == 0xFFFFFFFFu) return;

    smp_trampoline_cr3 = paging_kernel_directory();
    smp_trampoline_entry = (uint32_t)(uintptr_t)&smp_ap_main;

    uint32_t bsp_id = lapic_id();
    uint32_t target_count = cpu_count_hint();
    if (target_count > LIONOS_MAX_CPUS) target_count = LIONOS_MAX_CPUS;

    /* QEMU/xAPIC exposes additional CPUs with contiguous APIC IDs. */
    for (uint32_t index = 1u; index < target_count; ++index) {
        uint32_t stack_pages = LIONOS_SMP_STACK_PAGES;
        ap_stacks[index] = (uint32_t)(uintptr_t)page_alloc_contiguous(stack_pages);
        if (!ap_stacks[index]) break;

        smp_trampoline_stack = ap_stacks[index] + stack_pages * 4096u;
        smp_trampoline_cpu = index;
        uint32_t target_apic = bsp_id + index;

        lapic_send_init(target_apic);
        delay(100000u);
        lapic_send_startup(target_apic, LIONOS_SMP_TRAMPOLINE >> 12);
        delay(200000u);
        lapic_send_startup(target_apic, LIONOS_SMP_TRAMPOLINE >> 12);

        if (wait_for_online(index) == 0) {
            ++online_count;
            console_write("[ OK ] CPU ");
            console_write_dec(index);
            console_write(" online\n");
            debug_write("LIONOS:SMP-CPU-ONLINE\n");
        } else {
            console_write("[ -- ] CPU ");
            console_write_dec(index);
            console_write(" AP startup timeout\n");
            debug_write("LIONOS:SMP-CPU-TIMEOUT\n");
            break;
        }
    }
}

uint32_t smp_online_count(void) { return online_count; }
