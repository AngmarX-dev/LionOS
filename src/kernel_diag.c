#include <stdint.h>
#include "kernel_diag.h"
#include "console.h"
#include "debug.h"
#include "cpu.h"

#define KLOG_LEVELS 5u
#define KLOG_VECTORS 256u
#define KLOG_RING_CAPACITY 64u
#define KLOG_MESSAGE_MAX 96u

struct kernel_log_entry {
    volatile uint32_t sequence;
    enum kernel_log_level level;
    char message[KLOG_MESSAGE_MAX];
};

static struct kernel_log_entry log_ring[KLOG_RING_CAPACITY];
static volatile uint32_t log_sequence;

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

    /* Bounded, allocation-free history for the built-in desktop log viewer. */
    uint32_t sequence = __atomic_add_fetch(&log_sequence, 1u, __ATOMIC_RELAXED);
    struct kernel_log_entry *entry = &log_ring[sequence % KLOG_RING_CAPACITY];
    __atomic_store_n(&entry->sequence, 0u, __ATOMIC_RELEASE);
    entry->level = level;
    uint32_t i = 0u;
    if (message) {
        while (message[i] && i + 1u < KLOG_MESSAGE_MAX) {
            entry->message[i] = message[i];
            ++i;
        }
    }
    entry->message[i] = 0;
    __atomic_store_n(&entry->sequence, sequence, __ATOMIC_RELEASE);

    debug_write("LIONOS:LOG:");
    debug_write(level_name(level));
    debug_write(":");
    if (message) debug_write(message);
    debug_write("\n");
}

uint32_t kernel_log_entry_count(void) {
    uint32_t count = __atomic_load_n(&log_sequence, __ATOMIC_ACQUIRE);
    return count > KLOG_RING_CAPACITY ? KLOG_RING_CAPACITY : count;
}

int kernel_log_read(uint32_t index, enum kernel_log_level *level,
                    char *message, uint32_t capacity) {
    if (!level || !message || capacity == 0u) return -1;
    uint32_t head = __atomic_load_n(&log_sequence, __ATOMIC_ACQUIRE);
    uint32_t count = head > KLOG_RING_CAPACITY ? KLOG_RING_CAPACITY : head;
    if (index >= count) return -1;

    uint32_t sequence = (head > KLOG_RING_CAPACITY
        ? head - KLOG_RING_CAPACITY + 1u : 1u) + index;
    struct kernel_log_entry *entry = &log_ring[sequence % KLOG_RING_CAPACITY];
    if (__atomic_load_n(&entry->sequence, __ATOMIC_ACQUIRE) != sequence)
        return -1;
    *level = entry->level;
    uint32_t i = 0u;
    while (i + 1u < capacity && i + 1u < KLOG_MESSAGE_MAX &&
           entry->message[i]) {
        message[i] = entry->message[i];
        ++i;
    }
    message[i] = 0;
    if (__atomic_load_n(&entry->sequence, __ATOMIC_ACQUIRE) != sequence)
        return -1;
    return 0;
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
