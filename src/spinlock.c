#include <stdint.h>
#include "spinlock.h"

void spinlock_init(struct spinlock *lock) {
    if (lock) lock->value = 0u;
}

uint32_t spinlock_try_acquire(struct spinlock *lock) {
    if (!lock) return 0u;
    uint32_t expected = 0u;
    return __sync_bool_compare_and_swap(&lock->value, expected, 1u) ? 1u : 0u;
}

void spinlock_acquire(struct spinlock *lock) {
    if (!lock) return;
    __asm__ volatile("cli" ::: "memory");
    while (!spinlock_try_acquire(lock)) __asm__ volatile("pause");
    __sync_synchronize();
}

void spinlock_release(struct spinlock *lock) {
    if (!lock) return;
    __sync_synchronize();
    __atomic_store_n(&lock->value, 0u, __ATOMIC_RELEASE);
    __asm__ volatile("sti" ::: "memory");
}
