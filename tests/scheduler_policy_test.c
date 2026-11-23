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
    if (!scheduler_tick_delta_valid(0u)) return 10;
    if (!scheduler_tick_delta_valid(1u)) return 11;
    if (!scheduler_tick_delta_valid(0x7FFFFFFFu)) return 12;
    if (scheduler_tick_delta_valid(0x80000000u)) return 13;
    if (scheduler_tick_delta_valid(UINT32_MAX)) return 14;
    if (!scheduler_tick_deadline_reached(100u, 100u)) return 15;
    if (!scheduler_tick_deadline_reached(101u, 100u)) return 16;
    if (scheduler_tick_deadline_reached(99u, 100u)) return 17;
    if (!scheduler_tick_deadline_reached(1u, UINT32_MAX)) return 18;
    if (scheduler_tick_deadline_reached(UINT32_MAX, 1u)) return 19;
    if (!scheduler_tick_deadline_reached(0x7FFFFFFFu, 0u)) return 20;
    if (scheduler_tick_deadline_reached(0x80000000u, 0u)) return 21;
    if (scheduler_tick_deadline_reached(0u, 0x7FFFFFFFu)) return 22;
    uint32_t used = 0u;
    priority = 0u;
    quantum = scheduler_priority_quantum(priority);
    if (scheduler_tick_charge(&used, &priority, &quantum, 0u) != 0 ||
        used != 0u || priority != 0u || quantum != 2u) return 23;
    if (scheduler_tick_charge(&used, &priority, &quantum, 1u) != 0 ||
        used != 1u || priority != 0u || quantum != 2u) return 24;
    if (scheduler_tick_charge(&used, &priority, &quantum, 1u) != 0 ||
        used != 0u || priority != 1u || quantum != 4u) return 25;

    /* A delayed scheduling decision consumes all elapsed ticks but performs
       only one demotion before the caller switches away from this task. */
    used = 1u;
    if (scheduler_tick_charge(&used, &priority, &quantum, UINT32_MAX) != 0 ||
        used != 0u || priority != 2u || quantum != 8u) return 26;
    used = 7u;
    if (scheduler_tick_charge(&used, &priority, &quantum, 1u) != 0 ||
        used != 0u || priority != 3u || quantum != 12u) return 27;
    used = 11u;
    if (scheduler_tick_charge(&used, &priority, &quantum, 1u) != 0 ||
        used != 0u || priority != 3u || quantum != 12u) return 28;

    used = 1u;
    priority = 1u;
    quantum = 4u;
    if (scheduler_tick_charge(0, &priority, &quantum, 1u) == 0 ||
        used != 1u || priority != 1u || quantum != 4u) return 29;
    if (scheduler_tick_charge(&used, 0, &quantum, 1u) == 0 ||
        used != 1u || priority != 1u || quantum != 4u) return 30;
    if (scheduler_tick_charge(&used, &priority, 0, 1u) == 0 ||
        used != 1u || priority != 1u || quantum != 4u) return 31;
    used = 4u;
    if (scheduler_tick_charge(&used, &priority, &quantum, 1u) == 0 ||
        used != 4u || priority != 1u || quantum != 4u) return 32;
    used = 0u;
    priority = 0u;
    quantum = 0u;
    if (scheduler_tick_charge(&used, &priority, &quantum, 1u) != 0 ||
        used != 0u || priority != 1u || quantum != 4u) return 33;
    used = 1u;
    if (scheduler_tick_charge(&used, &priority, &quantum, 1u) == 0 ||
        used != 1u || priority != 1u || quantum != 4u) return 34;
    return 0;
}
