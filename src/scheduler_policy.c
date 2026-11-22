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
