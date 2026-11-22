#include <stdint.h>
#include "scheduler_policy.h"

int main(void) {
    static const uint32_t expected_quantum[LIONOS_SCHEDULER_PRIORITY_LEVELS] = {
        2u, 4u, 8u, 12u
    };

    for (uint32_t priority = 0u;
         priority < LIONOS_SCHEDULER_PRIORITY_LEVELS; ++priority) {
        if (scheduler_priority_quantum(priority) != expected_quantum[priority])
            return 1;
    }
    if (scheduler_priority_quantum(UINT32_MAX) != 12u)
        return 2;

    uint32_t priority = 0u, quantum = 99u;
    if (scheduler_priority_demote(&priority, &quantum) != 0 ||
        priority != 1u || quantum != 4u)
        return 3;
    if (scheduler_priority_demote(&priority, &quantum) != 0 ||
        priority != 2u || quantum != 8u)
        return 4;
    if (scheduler_priority_demote(&priority, &quantum) != 0 ||
        priority != 3u || quantum != 12u)
        return 5;
    if (scheduler_priority_demote(&priority, &quantum) != 0 ||
        priority != 3u || quantum != 12u)
        return 6;

    priority = 2u;
    quantum = 77u;
    if (scheduler_priority_demote(0, &quantum) == 0 ||
        priority != 2u || quantum != 77u)
        return 7;
    if (scheduler_priority_demote(&priority, 0) == 0 ||
        priority != 2u || quantum != 77u)
        return 8;
    priority = UINT32_MAX;
    quantum = 55u;
    if (scheduler_priority_demote(&priority, &quantum) == 0 ||
        priority != UINT32_MAX || quantum != 55u)
        return 9;
    if (!scheduler_tick_deadline_reached(100u, 100u)) return 10;
    if (!scheduler_tick_deadline_reached(101u, 100u)) return 11;
    if (scheduler_tick_deadline_reached(99u, 100u)) return 12;
    if (!scheduler_tick_deadline_reached(1u, UINT32_MAX)) return 13;
    if (scheduler_tick_deadline_reached(UINT32_MAX, 1u)) return 14;
    if (!scheduler_tick_deadline_reached(0x7FFFFFFFu, 0u)) return 15;
    if (scheduler_tick_deadline_reached(0x80000000u, 0u)) return 16;
    if (scheduler_tick_deadline_reached(0u, 0x7FFFFFFFu)) return 17;
    return 0;
}
