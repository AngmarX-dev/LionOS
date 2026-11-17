#include <stdint.h>
#include "spinlock.h"
#include "process.h"

uint32_t tty_test_sleep_count;
uint32_t tty_test_timed_sleep_count;
uint32_t tty_test_last_timeout_ticks;
uint32_t tty_test_timeout_result;
uint32_t tty_test_wakeup_count;
uint32_t tty_test_signal_count;
uint32_t tty_test_signal_sid;
uint32_t tty_test_signal_pgid;
uint32_t tty_test_signal_number;
char tty_test_output[2048];
uint32_t tty_test_output_length;

void spinlock_init(struct spinlock *lock) { if (lock) lock->value = 0u; }
void spinlock_acquire(struct spinlock *lock) { if (lock) lock->value = 1u; }
void spinlock_release(struct spinlock *lock) { if (lock) lock->value = 0u; }
uint32_t spinlock_try_acquire(struct spinlock *lock) {
    if (!lock || lock->value) return 0u;
    lock->value = 1u;
    return 1u;
}
uint32_t spinlock_irqsave_acquire(struct spinlock *lock) {
    spinlock_acquire(lock);
    return 0u;
}
void spinlock_irqrestore_release(struct spinlock *lock, uint32_t flags) {
    (void)flags;
    spinlock_release(lock);
}
void console_write_n(const char *buffer, uint32_t length) {
    if (!buffer || length > sizeof(tty_test_output) - tty_test_output_length)
        return;
    for (uint32_t i = 0u; i < length; ++i)
        tty_test_output[tty_test_output_length++] = buffer[i];
}
int32_t process_sleep_on(uintptr_t channel, struct spinlock *held_lock, uint32_t irq_flags) {
    if (!channel || !held_lock) return -1;
    ++tty_test_sleep_count;
    spinlock_irqrestore_release(held_lock, irq_flags);
    return PROCESS_WAIT_BLOCKED;
}
int32_t process_sleep_on_timeout(uintptr_t channel, struct spinlock *held_lock,
                                 uint32_t irq_flags, uint32_t timeout_ticks) {
    if (!channel || !held_lock || !timeout_ticks) return -1;
    ++tty_test_timed_sleep_count;
    tty_test_last_timeout_ticks = timeout_ticks;
    spinlock_irqrestore_release(held_lock, irq_flags);
    return PROCESS_WAIT_BLOCKED;
}
int32_t process_consume_timeout(void) {
    uint32_t result = tty_test_timeout_result;
    tty_test_timeout_result = 0u;
    return result ? 1 : 0;
}
void process_wakeup(uintptr_t channel) { if (channel) ++tty_test_wakeup_count; }
int32_t process_tty_signal_foreground(uint32_t sid, uint32_t pgid, uint32_t signal) {
    if (!sid || !pgid || !signal) return -1;
    ++tty_test_signal_count;
    tty_test_signal_sid = sid;
    tty_test_signal_pgid = pgid;
    tty_test_signal_number = signal;
    return 0;
}
