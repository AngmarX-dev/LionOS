#include "scheduler_policy.h"

uint32_t scheduler_priority_quantum(uint32_t priority) {
    static const uint32_t quantum[LIONOS_SCHEDULER_PRIORITY_LEVELS] = {
        2u, 4u, 8u, 12u
    };

    if (priority >= LIONOS_SCHEDULER_PRIORITY_LEVELS)
        priority = LIONOS_SCHEDULER_PRIORITY_LEVELS - 1u;
    return quantum[priority];
}

int scheduler_priority_demote(uint32_t *priority, uint32_t *time_slice) {
    if (!priority || !time_slice ||
        *priority >= LIONOS_SCHEDULER_PRIORITY_LEVELS)
        return -1;

    if (*priority < LIONOS_SCHEDULER_PRIORITY_LEVELS - 1u)
        ++*priority;
    *time_slice = scheduler_priority_quantum(*priority);
    return 0;
}

int scheduler_tick_charge(uint32_t *ticks_used, uint32_t *priority,
                          uint32_t *time_slice, uint32_t elapsed_ticks) {
    if (!ticks_used || !priority || !time_slice ||
        *priority >= LIONOS_SCHEDULER_PRIORITY_LEVELS)
        return -1;

    if (elapsed_ticks == 0u) return 0;

    /* The bootstrap context starts with a zero quantum. Preserve the legacy
       behavior: its first charged scheduler tick establishes a real quantum
       by demoting once, rather than treating initialization as corruption. */
    if (*time_slice == 0u) {
        if (*ticks_used != 0u) return -1;
        return scheduler_priority_demote(priority, time_slice);
    }
    if (*ticks_used >= *time_slice) return -1;

    uint32_t remaining = *time_slice - *ticks_used;
    if (elapsed_ticks < remaining) {
        *ticks_used += elapsed_ticks;
        return 0;
    }

    /* The caller is about to leave this task; do not carry timer overrun
       into the next quantum or demote more than one level in one decision. */
    *ticks_used = 0u;
    return scheduler_priority_demote(priority, time_slice);
}

int scheduler_tick_accumulate(volatile uint32_t *pending_ticks) {
    if (!pending_ticks) return -1;

    uint32_t pending = __atomic_load_n(pending_ticks, __ATOMIC_RELAXED);
    for (;;) {
        if (pending == UINT32_MAX) return 0;

        /* pending is strictly below UINT32_MAX, so this increment cannot wrap. */
        uint32_t next = pending + 1u;
        if (__atomic_compare_exchange_n(pending_ticks, &pending, next, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            return 1;
    }
}

int scheduler_tick_delta_valid(uint32_t ticks) {
    return ticks <= 0x7FFFFFFFu;
}

int scheduler_tick_deadline_reached(uint32_t now, uint32_t deadline) {
    /*
     * Unsigned subtraction is defined modulo 2^32. A result in the lower
     * half-range means the deadline is reached; the upper half means it is
     * still in the future. Callers must cap relative deadlines at INT32_MAX.
     */
    return (uint32_t)(now - deadline) <= 0x7FFFFFFFu;
}
