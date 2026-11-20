#include "debug.h"
#include "io.h"

#define DEBUGCON_PORT 0xE9u

/*
 * Debugcon is a byte stream shared by every CPU. Keep each debug_write()
 * record contiguous so SMP diagnostics and CI markers cannot interleave.
 * Disable local maskable interrupts before taking the lock to avoid
 * self-deadlock if an interrupt logs while the interrupted CPU owns it.
 */
static volatile uint32_t debug_write_lock;

void debug_putc(char c) {
    outb(DEBUGCON_PORT, (uint8_t)c);
}

void debug_write(const char *s) {
    uint32_t interrupt_flags;

    __asm__ volatile("pushfl; popl %0; cli"
                     : "=r"(interrupt_flags)
                     :
                     : "memory");

    while (__atomic_exchange_n(&debug_write_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
        __asm__ volatile("pause" ::: "memory");

    while (*s)
        debug_putc(*s++);

    __atomic_store_n(&debug_write_lock, 0u, __ATOMIC_RELEASE);

    __asm__ volatile("pushl %0; popfl"
                     :
                     : "r"(interrupt_flags)
                     : "memory", "cc");
}
