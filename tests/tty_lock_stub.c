#include <stdint.h>
#include "spinlock.h"

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
void console_write_n(const char *buffer, uint32_t length) { (void)buffer; (void)length; }
