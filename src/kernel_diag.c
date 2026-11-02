#include <stdint.h>
#include "kernel_diag.h"
#include "console.h"
#include "debug.h"
#include "cpu.h"
#include "lapic.h"

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
    kernel_log_ring_record(level, message);
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

static void debug_hex32(uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";
    debug_write("0x");
    for (int shift=28; shift>=0; shift-=4)
        debug_putc(digits[(value >> (uint32_t)shift) & 0xFu]);
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
    if (vector == 14u) {
        uint32_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        console_write(" CR2=");
        console_write_hex(cr2);
    }
    console_write("\nSystem halted.\n");
    debug_write("LIONOS:PANIC vector=");
    debug_hex32(vector);
    debug_write(" error=");
    debug_hex32(error);
    debug_write(" eip=");
    debug_hex32(eip);
    if (vector == 14u) {
        uint32_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        debug_write(" cr2=");
        debug_hex32(cr2);
    }
    debug_write(" cs=");
    debug_hex32(cs);
    debug_write(" eflags=");
    debug_hex32(eflags);
    debug_write("\n");
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
    console_write("       interrupt counters: deferred until CPU interrupts are enabled\n");
    console_write("       online CPUs=");
    uint32_t online=0u;
    for (uint32_t i=0;i<LIONOS_MAX_CPUS;++i) {
        const struct cpu_info *c=cpu_get(i);
        if (c && c->online) ++online;
    }
    console_write_dec(online);
    console_write("\n");
}
static void debug_dec32(uint32_t value) {
    char digits[10];
    uint32_t count = 0u;
    if (value == 0u) {
        debug_putc('0');
        return;
    }
    while (value != 0u && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (count != 0u)
        debug_putc(digits[--count]);
}

void kernel_diag_print_interrupts(void) {
    uint32_t timer = kernel_diag_interrupt_count(0u,32u);
    uint32_t keyboard = kernel_diag_interrupt_count(0u,33u);
    uint32_t mouse = kernel_diag_interrupt_count(0u,44u);
    uint32_t lapic = kernel_diag_interrupt_count(0u,48u);

    console_write("[ OK ] Interrupt counters after IRQ enable (BSP): timer=");
    console_write_dec(timer);
    console_write(" keyboard=");
    console_write_dec(keyboard);
    console_write(" mouse=");
    console_write_dec(mouse);
    console_write(" LAPIC=");
    console_write_dec(lapic);
    console_write("\n");

    /* Preserve the post-STI sample even if the desktop redraws the console. */
    debug_write("LIONOS:IRQ-COUNTS timer=");
    debug_dec32(timer);
    debug_write(" keyboard=");
    debug_dec32(keyboard);
    debug_write(" mouse=");
    debug_dec32(mouse);
    debug_write(" lapic=");
    debug_dec32(lapic);
    debug_write("\n");
}

void kernel_diag_record_interrupt(uint32_t cpu, uint32_t vector) {
    if (cpu < LIONOS_MAX_CPUS && vector < KLOG_VECTORS)
        __atomic_fetch_add(&interrupt_counts[cpu][vector], 1u, __ATOMIC_RELAXED);
}
uint32_t kernel_diag_interrupt_count(uint32_t cpu, uint32_t vector) {
    if (cpu >= LIONOS_MAX_CPUS || vector >= KLOG_VECTORS) return 0u;
    return __atomic_load_n(&interrupt_counts[cpu][vector], __ATOMIC_RELAXED);
}


int kernel_diag_validate_timers(void) {
    if (!lapic_frame_timer_active()) {
        console_write("[WARN] Timer validation skipped: LAPIC frame clock unavailable\n");
        debug_write("LIONOS:TIMER-VALIDATION-SKIPPED\n");
        return -1;
    }

    const uint32_t start_frame = lapic_frame_ticks();
    const uint32_t start_pit = kernel_diag_interrupt_count(0u, 32u);
    const uint32_t start_lapic = kernel_diag_interrupt_count(0u, 48u);

    /* Use the calibrated 120 Hz LAPIC clock as the two-second reference. */
    while ((uint32_t)(lapic_frame_ticks() - start_frame) < 240u)
        __asm__ volatile("hlt");

    const uint32_t frame_delta = (uint32_t)(lapic_frame_ticks() - start_frame);
    const uint32_t pit_delta =
        (uint32_t)(kernel_diag_interrupt_count(0u, 32u) - start_pit);
    const uint32_t lapic_delta =
        (uint32_t)(kernel_diag_interrupt_count(0u, 48u) - start_lapic);

    /* Broad +/-20% windows tolerate timer delivery jitter in virtual machines. */
    const int frame_ok = frame_delta >= 240u && frame_delta <= 264u;
    const int pit_ok = pit_delta >= 160u && pit_delta <= 240u;
    const int lapic_ok = lapic_delta >= 192u && lapic_delta <= 288u;

    console_write("[TIMER] 2-second sample: PIT=");
    console_write_dec(pit_delta);
    console_write("/200 LAPIC=");
    console_write_dec(lapic_delta);
    console_write("/240 FRAME=");
    console_write_dec(frame_delta);
    console_write("/240\n");

    debug_write("LIONOS:TIMER-SAMPLE pit=");
    debug_dec32(pit_delta);
    debug_write(" lapic=");
    debug_dec32(lapic_delta);
    debug_write(" frame=");
    debug_dec32(frame_delta);
    debug_write("\n");

    if (frame_ok && pit_ok && lapic_ok) {
        console_write("[ OK ] PIT/LAPIC timer delivery within tolerance\n");
        debug_write("LIONOS:TIMER-VALIDATION-PASS\n");
        return 0;
    }

    console_write("[WARN] Timer delivery outside expected tolerance\n");
    debug_write("LIONOS:TIMER-VALIDATION-WARN\n");
    return -1;
}
