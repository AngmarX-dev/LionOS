#ifndef LIONOS_SCHEDULER_POLICY_H
#define LIONOS_SCHEDULER_POLICY_H

#include <stdint.h>

#define LIONOS_SCHEDULER_PRIORITY_LEVELS 4u

/* Lower numeric values are higher priority. */
uint32_t scheduler_priority_quantum(uint32_t priority);

/*
 * Demote one level after a complete time slice and refresh the quantum.
 * Returns -1 without modifying either value for invalid pointers/priority.
 */
int scheduler_priority_demote(uint32_t *priority, uint32_t *time_slice);

/* Relative deadlines must stay within the unambiguous unsigned half-range. */
int scheduler_tick_delta_valid(uint32_t ticks);

/* Wrap-safe for deadlines scheduled less than 2^31 ticks ahead. */
int scheduler_tick_deadline_reached(uint32_t now, uint32_t deadline);

#endif
