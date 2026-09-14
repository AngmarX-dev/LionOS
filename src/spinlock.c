#include <stdint.h>
#include "spinlock.h"

void spinlock_init(struct spinlock *lock) { if (lock) lock->value = 0u; }

uint32_t spinlock_try_acquire(struct spinlock *lock) {
    if (!lock) return 0u;
    return __sync_bool_compare_and_swap(&lock->value, 0u, 1u) ? 1u : 0u;
}

void spinlock_acquire(struct spinlock *lock) {
    if (!lock) return;
    while (!spinlock_try_acquire(lock)) __asm__ volatile("pause");
    __sync_synchronize();
}

void spinlock_release(struct spinlock *lock) {
    if (!lock) return;
    __sync_synchronize();
    __atomic_store_n(&lock->value, 0u, __ATOMIC_RELEASE);
}

uint32_t spinlock_irqsave_acquire(struct spinlock *lock) {
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    spinlock_acquire(lock);
    return flags;
}

void spinlock_irqrestore_release(struct spinlock *lock, uint32_t flags) {
    spinlock_release(lock);
    __asm__ volatile("pushl %0; popfl" : : "r"(flags) : "memory","cc");
}
