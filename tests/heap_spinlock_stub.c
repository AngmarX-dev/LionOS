#include <stdint.h>
#include <sched.h>
#include "spinlock.h"

void spinlock_init(struct spinlock *lock)
{
    if (lock)
        __atomic_store_n(&lock->value, 0u, __ATOMIC_RELAXED);
}
uint32_t spinlock_try_acquire(struct spinlock *lock)
{
    uint32_t expected = 0u;
    if (!lock)
        return 0u;
    return __atomic_compare_exchange_n(&lock->value, &expected, 1u, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
void spinlock_acquire(struct spinlock *lock)
{
    while (!spinlock_try_acquire(lock))
        sched_yield();
}
void spinlock_release(struct spinlock *lock)
{
    if (lock)
        __atomic_store_n(&lock->value, 0u, __ATOMIC_RELEASE);
}
uint32_t spinlock_irqsave_acquire(struct spinlock *lock)
{
    spinlock_acquire(lock);
    return 0u;
}
void spinlock_irqrestore_release(struct spinlock *lock, uint32_t flags)
{
    (void)flags;
    spinlock_release(lock);
}
