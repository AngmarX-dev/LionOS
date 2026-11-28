#ifndef LIONOS_SCHEDULER_POLICY_H
#define LIONOS_SCHEDULER_POLICY_H

#include <stdint.h>

#define LIONOS_SCHEDULER_PRIORITY_LEVELS 4u

/* Lower numeric values are higher priority. */
uint32_t scheduler_priority_quantum(uint32_t priority);

/*
 * Select the largest eligible queue, breaking equal-backlog ties in cyclic
 * order from start_cpu. visited_mask excludes CPUs already examined during
 * this balancing attempt. Supports at most 32 CPUs; UINT32_MAX means none.
 */
uint32_t scheduler_select_victim(const uint32_t *queue_totals,
                                 uint32_t online_cpus,
                                 uint32_t destination_cpu,
                                 uint32_t start_cpu,
                                 uint32_t visited_mask);

/*
 * Demote one level after a complete time slice and refresh the quantum.
 * Returns -1 without modifying either value for invalid pointers/priority.
 */
int scheduler_priority_demote(uint32_t *priority, uint32_t *time_slice);

/*
 * Charge elapsed local timer ticks to the running task. When the current
 * quantum expires, reset ticks_used and demote exactly one priority level;
 * the caller must schedule another task after a successful expiration.
 * Invalid state is rejected without modifying any argument.
 */
int scheduler_tick_charge(uint32_t *ticks_used, uint32_t *priority,
                          uint32_t *time_slice, uint32_t elapsed_ticks);

/*
 * Atomically increment a per-CPU clock/diagnostic counter. Returns the
 * incremented value; unsigned wraparound is intentional. counter must be
 * a valid pointer to a uint32_t object.
 */
uint32_t scheduler_counter_increment(volatile uint32_t *counter);

/*
 * Atomically add one pending scheduler tick without wrapping. Returns 1 when
 * incremented, 0 when already saturated, and -1 for a null counter.
 * Safe for concurrent timer producers; the scheduler consumes the counter
 * with an atomic exchange after entering its scheduling critical section.
 */
int scheduler_tick_accumulate(volatile uint32_t *pending_ticks);

/* Relative deadlines and elapsed epochs must stay within the unsigned half-range. */
int scheduler_tick_delta_valid(uint32_t ticks);

/*
 * Calculate a wrap-safe 32-bit epoch delta. Returns -1 for a null output or
 * an ambiguous interval greater than INT32_MAX; on failure delta_out is
 * left unchanged. The caller must ensure the real interval is < 2^31 steps.
 */
int scheduler_epoch_delta(uint32_t earlier, uint32_t later,
                          uint32_t *delta_out);

/* Wrap-safe for deadlines scheduled less than 2^31 ticks ahead. */
int scheduler_tick_deadline_reached(uint32_t now, uint32_t deadline);

#endif
