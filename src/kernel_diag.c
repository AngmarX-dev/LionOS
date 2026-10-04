#include <stdint.h>
#include "kernel_diag.h"
#include "console.h"
#include "debug.h"
#include "cpu.h"

#define KLOG_LEVELS 5u
#define KLOG_VECTORS 256u

static volatile uint32_t log_counts[KLOG_LEVELS];
static volatile uint32_t panic_active;
static volatile uint32_t interrupt_counts[LIONOS_MAX_CPUS][KLOG_VECTORS];

static const char *level_name(enum kernel_log_level level) {
    switch(level) {
        case KLOG_DEBUG: return "DEBUG";
        case KLOG_INFO: return "INFO";
        case KLOG_WARN: return "WARN";
        case KLOG_ERROR: return "ERROR";
        case KLOG_PANIC: return "PANIC";
        default: return "UNKNOWN";
    }
}

void kernel_log(enum kernel_log_level level, const char *message) {
    if ((uint32_t)level >= KLOG_LEVELS) level = KLOG_ERROR;
    __atomic_fetch_add(&log_counts[(uint32_t)level], 1u, __ATOMIC_RELAXED);
    debug_write("LIONOS:LOG:");
    debug_write(level_name(level));
    debug_write(":");
    if (message) debug_write(message);
    debug_write("\n");
}

uint32_t kernel_diag_log_count(enum kernel_log_level level) {
    if ((uint32_t)level >= KLOG_LEVELS) return 0u;
    return __atomic_load_n(&log_counts[(uint32_t)level], __ATOMIC_RELAXED);
}

void kernel_panic(uint32_t vector, uint32_t error, uint32_t eip, uint32_t cs, uint32_t eflags) {
    uint32_t expected = 0u;
    if (!__atomic_compare_exchange_n(&panic_active, &expected, 1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        for (;;) __asm__ volatile("cli; hlt");

    kernel_log(KLOG_PANIC, "kernel exception");
    console_write("\n[FATAL] KERNEL PANIC\n");
    console_write(" vector="); console_write_dec(vector);
    console_write(" error=0x"); console_write_hex(error);
    console_write(" eip=0x"); console_write_hex(eip);
    console_write(" cs=0x"); console_write_hex(cs);
    console_write(" eflags=0x"); console_write_hex(eflags);
    console_write("\nSystem halted.\n");
    debug_write("LIONOS:PANIC\n");
    for (;;) __asm__ volatile("cli; hlt");
}

void kernel_diag_print(void) {
    console_write("[ OK ] Kernel diagnostics: log levels DEBUG/INFO/WARN/ERROR/PANIC\n");
    console_write("       log counts: ");
    for (uint32_t i=0;i<KLOG_LEVELS;++i) {
        if (i) console_write(" / ");
        console_write(level_name((enum kernel_log_level)i));
        console_write("=");
        console_write_dec(kernel_diag_log_count((enum kernel_log_level)i));
    }
    console_write("\n");
    console_write("       interrupt counters (BSP): timer=");
    console_write_dec(kernel_diag_interrupt_count(0u,32u));
    console_write(" keyboard=");
    console_write_dec(kernel_diag_interrupt_count(0u,33u));
    console_write(" mouse=");
    console_write_dec(kernel_diag_interrupt_count(0u,44u));
    console_write(" LAPIC=");
    console_write_dec(kernel_diag_interrupt_count(0u,48u));
    console_write("\n");
    console_write("       online CPUs=");
    uint32_t online=0u;
    for (uint32_t i=0;i<LIONOS_MAX_CPUS;++i) {
        const struct cpu_info *c=cpu_get(i);
        if (c && c->online) ++online;
    }
    console_write_dec(online);
    console_write("\n");
}
void kernel_diag_record_interrupt(uint32_t cpu, uint32_t vector) {
    if (cpu < LIONOS_MAX_CPUS && vector < KLOG_VECTORS)
        __atomic_fetch_add(&interrupt_counts[cpu][vector], 1u, __ATOMIC_RELAXED);
}
uint32_t kernel_diag_interrupt_count(uint32_t cpu, uint32_t vector) {
    if (cpu >= LIONOS_MAX_CPUS || vector >= KLOG_VECTORS) return 0u;
    return __atomic_load_n(&interrupt_counts[cpu][vector], __ATOMIC_RELAXED);
}
