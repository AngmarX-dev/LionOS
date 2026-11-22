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
