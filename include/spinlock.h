#ifndef LIONOS_SPINLOCK_H
#define LIONOS_SPINLOCK_H

#include <stdint.h>

struct spinlock {
    volatile uint32_t value;
};

void spinlock_init(struct spinlock *lock);
void spinlock_acquire(struct spinlock *lock);
void spinlock_release(struct spinlock *lock);
uint32_t spinlock_try_acquire(struct spinlock *lock);

#endif
