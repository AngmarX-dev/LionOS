#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include "scheduler_policy.h"

struct accumulator_worker {
    uint32_t iterations;
    uint32_t successes;
    uint32_t errors;
};

static volatile uint32_t pending_ticks;
static volatile uint32_t accumulator_ready;
static volatile uint32_t accumulator_start;

static void *accumulator_worker_main(void *argument) {
    struct accumulator_worker *worker = argument;
    (void)__atomic_fetch_add(&accumulator_ready, 1u, __ATOMIC_RELEASE);
    while (__atomic_load_n(&accumulator_start, __ATOMIC_ACQUIRE) == 0u) {
        /* Wait until every successfully created worker is ready to contend. */
    }
    for (uint32_t i = 0u; i < worker->iterations; ++i) {
        int result = scheduler_tick_accumulate(&pending_ticks);
        if (result == 1)
            ++worker->successes;
        else if (result < 0)
            ++worker->errors;
    }
    return 0;
}

/*
 * Model timer ticks that arrive while process_schedule() cannot acquire
 * process_lock. A failed try-lock must leave the pending count untouched;
 * the next successful scheduling decision drains and charges it exactly once.
 */
struct tick_drain_stress {
    volatile uint32_t pending;
    volatile uint32_t ready;
    volatile uint32_t start;
    volatile uint32_t producers_done;
    volatile uint32_t accepted;
    volatile uint32_t saturated;
    volatile uint32_t saturation_seen;
    volatile uint32_t consumed;
    volatile uint32_t errors;
    uint32_t initial_pending;
    uint32_t wait_for_saturation;
};

struct tick_drain_producer {
    struct tick_drain_stress *stress;
    uint32_t iterations;
};

static void *tick_drain_producer_main(void *argument) {
    struct tick_drain_producer *producer = argument;
    struct tick_drain_stress *stress = producer->stress;

    (void)__atomic_fetch_add(&stress->ready, 1u, __ATOMIC_RELEASE);
    while (__atomic_load_n(&stress->start, __ATOMIC_ACQUIRE) == 0u)
        sched_yield();

    for (uint32_t i = 0u; i < producer->iterations; ++i) {
        int result = scheduler_tick_accumulate(&stress->pending);
        if (result == 1) {
            (void)__atomic_fetch_add(&stress->accepted, 1u,
                                     __ATOMIC_RELAXED);
        } else if (result == 0) {
            (void)__atomic_fetch_add(&stress->saturated, 1u,
                                     __ATOMIC_RELAXED);
            __atomic_store_n(&stress->saturation_seen, 1u, __ATOMIC_RELEASE);
        } else {
            (void)__atomic_fetch_add(&stress->errors, 1u,
                                     __ATOMIC_RELAXED);
        }
    }
    (void)__atomic_fetch_add(&stress->producers_done, 1u, __ATOMIC_RELEASE);
    return 0;
}

static void *tick_drain_consumer_main(void *argument) {
    struct tick_drain_stress *stress = argument;

    uint32_t initial_pending = stress->initial_pending;

    (void)__atomic_fetch_add(&stress->ready, 1u, __ATOMIC_RELEASE);
    while (__atomic_load_n(&stress->start, __ATOMIC_ACQUIRE) == 0u)
        sched_yield();
    while (stress->wait_for_saturation != 0u &&
           __atomic_load_n(&stress->saturation_seen, __ATOMIC_ACQUIRE) == 0u)
        sched_yield();

    for (;;) {
        uint32_t elapsed = __atomic_exchange_n(&stress->pending, 0u,
                                                __ATOMIC_ACQ_REL);
        if (elapsed != 0u) {
            /*
             * The saturation variant seeds the counter near UINT32_MAX.
             * Exclude that fixture value from the produced/consumed totals
             * exactly once, even if producers have already added more ticks.
             */
            if (initial_pending != 0u) {
                if (elapsed < initial_pending) {
                    (void)__atomic_fetch_add(&stress->errors, 1u,
                                             __ATOMIC_RELAXED);
                    return 0;
                }
                elapsed -= initial_pending;
                initial_pending = 0u;
            }
            if (elapsed != 0u)
                (void)__atomic_fetch_add(&stress->consumed, elapsed,
                                         __ATOMIC_RELAXED);
        } else {
            sched_yield();
        }

        if (__atomic_load_n(&stress->producers_done, __ATOMIC_ACQUIRE) == 8u &&
            __atomic_load_n(&stress->pending, __ATOMIC_ACQUIRE) == 0u)
            break;
    }
    return 0;
}

/*
 * Race multiple timer producers against the scheduler's atomic drain. Every
 * accepted tick must be consumed exactly once, including ticks arriving while
 * the consumer is exchanging the pending counter.
 */
static int concurrent_tick_drain_test(uint32_t iterations,
                                         uint32_t initial_pending,
                                         int wait_for_saturation) {
    enum { PRODUCERS = 8 };
    pthread_t producers[PRODUCERS];
    pthread_t consumer;
    struct tick_drain_producer workers[PRODUCERS];
    struct tick_drain_stress stress = {
        initial_pending, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        initial_pending, wait_for_saturation != 0 ? 1u : 0u
    };
    uint32_t started = 0u;
    int failed = 0;

    if (iterations > UINT32_MAX / PRODUCERS ||
        (wait_for_saturation == 0 &&
         initial_pending > UINT32_MAX - iterations * PRODUCERS) ||
        (wait_for_saturation != 0 && initial_pending == 0u))
        return -1;

    for (; started < PRODUCERS; ++started) {
        workers[started].stress = &stress;
        workers[started].iterations = iterations;
        if (pthread_create(&producers[started], 0, tick_drain_producer_main,
                           &workers[started]) != 0)
            break;
    }
    if (started != PRODUCERS) {
        failed = 1;
        (void)__atomic_store_n(&stress.start, 1u, __ATOMIC_RELEASE);
        for (uint32_t i = 0u; i < started; ++i)
            if (pthread_join(producers[i], 0) != 0) failed = 1;
        return -1;
    }

    if (pthread_create(&consumer, 0, tick_drain_consumer_main, &stress) != 0) {
        (void)__atomic_store_n(&stress.start, 1u, __ATOMIC_RELEASE);
        for (uint32_t i = 0u; i < started; ++i)
            if (pthread_join(producers[i], 0) != 0) failed = 1;
        return -1;
    }

    while (__atomic_load_n(&stress.ready, __ATOMIC_ACQUIRE) !=
           PRODUCERS + 1u)
        sched_yield();
    __atomic_store_n(&stress.start, 1u, __ATOMIC_RELEASE);

    for (uint32_t i = 0u; i < started; ++i)
        if (pthread_join(producers[i], 0) != 0) failed = 1;
    if (pthread_join(consumer, 0) != 0) failed = 1;

    uint32_t expected = iterations * PRODUCERS;
    uint32_t accepted = __atomic_load_n(&stress.accepted, __ATOMIC_ACQUIRE);
    uint32_t saturated = __atomic_load_n(&stress.saturated, __ATOMIC_ACQUIRE);
    if (failed || __atomic_load_n(&stress.errors, __ATOMIC_ACQUIRE) != 0u ||
        __atomic_load_n(&stress.producers_done, __ATOMIC_ACQUIRE) != PRODUCERS ||
        accepted > expected || saturated != expected - accepted ||
        __atomic_load_n(&stress.consumed, __ATOMIC_ACQUIRE) != accepted ||
        __atomic_load_n(&stress.pending, __ATOMIC_ACQUIRE) != 0u ||
        (wait_for_saturation != 0 && saturated == 0u) ||
        (wait_for_saturation == 0 && accepted != expected))
        return -1;
    return 0;
}

static int deferred_tick_charge_after_contention_test(void) {
    volatile uint32_t pending = 0u;
    uint32_t used = 1u;
    uint32_t priority = 0u;
    uint32_t quantum = scheduler_priority_quantum(priority);

    if (scheduler_tick_accumulate(&pending) != 1 ||
        scheduler_tick_accumulate(&pending) != 1 ||
        scheduler_tick_accumulate(&pending) != 1)
        return 1;

    /* Simulate repeated try-lock failures: no consumer may clear the ticks. */
    if (__atomic_load_n(&pending, __ATOMIC_ACQUIRE) != 3u)
        return 2;

    uint32_t elapsed = __atomic_exchange_n(&pending, 0u, __ATOMIC_ACQ_REL);
    if (elapsed != 3u ||
        __atomic_load_n(&pending, __ATOMIC_ACQUIRE) != 0u)
        return 3;

    /* An overrun is charged once and causes only one priority demotion. */
    if (scheduler_tick_charge(&used, &priority, &quantum, elapsed) != 0 ||
        used != 0u || priority != 1u || quantum != 4u)
        return 4;

    /* A second successful entry must not charge the already-consumed ticks. */
    elapsed = __atomic_exchange_n(&pending, 0u, __ATOMIC_ACQ_REL);
    if (elapsed != 0u ||
        scheduler_tick_charge(&used, &priority, &quantum, elapsed) != 0 ||
        used != 0u || priority != 1u || quantum != 4u)
        return 5;

    /* Later ticks continue from the new quantum without losing elapsed time. */
    if (scheduler_tick_accumulate(&pending) != 1 ||
        scheduler_tick_accumulate(&pending) != 1)
        return 6;
    elapsed = __atomic_exchange_n(&pending, 0u, __ATOMIC_ACQ_REL);
    if (elapsed != 2u ||
        scheduler_tick_charge(&used, &priority, &quantum, elapsed) != 0 ||
        used != 2u || priority != 1u || quantum != 4u)
        return 7;

    return 0;
}

static int run_accumulator_workers(uint32_t iterations,
                                      uint32_t *successes_out) {
    enum { WORKERS = 8 };
    pthread_t threads[WORKERS];
    static struct accumulator_worker workers[WORKERS];
    uint32_t started = 0u;
    int failed = 0;

    if (!successes_out) return -1;
    *successes_out = 0u;
    __atomic_store_n(&accumulator_ready, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&accumulator_start, 0u, __ATOMIC_RELAXED);
    for (; started < WORKERS; ++started) {
        workers[started].iterations = iterations;
        workers[started].successes = 0u;
        workers[started].errors = 0u;
        if (pthread_create(&threads[started], 0, accumulator_worker_main,
                           &workers[started]) != 0)
            break;
    }
    if (started != WORKERS) failed = 1;

    /* Release all workers together only after each one has reached the gate. */
    while (__atomic_load_n(&accumulator_ready, __ATOMIC_ACQUIRE) < started) {
    }
    __atomic_store_n(&accumulator_start, 1u, __ATOMIC_RELEASE);

    for (uint32_t i = 0u; i < started; ++i) {
        if (pthread_join(threads[i], 0) != 0) {
            failed = 1;
            continue;
        }
        if (workers[i].errors != 0u ||
            UINT32_MAX - *successes_out < workers[i].successes)
            failed = 1;
        else
            *successes_out += workers[i].successes;
    }
    return failed ? -1 : 0;
}

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

    /* Equal-backlog victim selection rotates instead of pinning ties to CPU 1. */
    const uint32_t balanced_totals[4] = { 0u, 5u, 5u, 5u };
    if (scheduler_select_victim(balanced_totals, 4u, 0u, 0u, 0u) != 1u)
        return 41;
    if (scheduler_select_victim(balanced_totals, 4u, 0u, 2u, 0u) != 2u)
        return 42;
    if (scheduler_select_victim(balanced_totals, 4u, 0u, 3u, 0u) != 3u)
        return 43;
    if (scheduler_select_victim(balanced_totals, 4u, 0u, 2u,
                                1u << 2u) != 3u)
        return 44;
    const uint32_t unequal_totals[4] = { 0u, 3u, 7u, 7u };
    if (scheduler_select_victim(unequal_totals, 4u, 0u, 3u, 0u) != 3u)
        return 45;
    if (scheduler_select_victim(balanced_totals, 4u, 0u, 0u,
                                (1u << 1u) | (1u << 2u) | (1u << 3u)) !=
        UINT32_MAX)
        return 46;
    if (scheduler_select_victim(0, 4u, 0u, 0u, 0u) != UINT32_MAX ||
        scheduler_select_victim(balanced_totals, 33u, 0u, 0u, 0u) !=
        UINT32_MAX)
        return 47;

    /* Exercise cyclic tie-breaking and mask boundaries at maximum CPU counts. */
    uint32_t totals16[16];
    for (uint32_t cpu = 0u; cpu < 16u; ++cpu)
        totals16[cpu] = cpu == 0u ? 0u : 9u;
    if (scheduler_select_victim(totals16, 16u, 0u, 15u, 0u) != 15u)
        return 48;
    if (scheduler_select_victim(totals16, 16u, 0u, 0u, 0u) != 1u)
        return 49;

    uint32_t totals32[32];
    for (uint32_t cpu = 0u; cpu < 32u; ++cpu)
        totals32[cpu] = cpu == 0u ? 0u : 4u;
    if (scheduler_select_victim(totals32, 32u, 0u, 31u, 0u) != 31u)
        return 50;
    if (scheduler_select_victim(totals32, 32u, 0u, 31u,
                                1u << 31u) != 1u)
        return 51;
    if (scheduler_select_victim(totals32, 32u, 0u, 31u,
                                UINT32_MAX) != UINT32_MAX)
        return 52;
    for (uint32_t cpu = 0u; cpu < 32u; ++cpu)
        totals32[cpu] = 0u;
    if (scheduler_select_victim(totals32, 32u, 0u, 31u, 0u) !=
        UINT32_MAX)
        return 53;
    if (scheduler_select_victim(totals32, 32u, 32u, 0u, 0u) !=
        UINT32_MAX ||
        scheduler_select_victim(totals32, 32u, 0u, 32u, 0u) !=
        UINT32_MAX)
        return 54;

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

    /* Once bootstrap has acquired a valid quantum, ordinary charging resumes. */
    used = 1u;
    if (scheduler_tick_charge(&used, &priority, &quantum, 1u) != 0 ||
        used != 2u || priority != 1u || quantum != 4u) return 34;

    /* A zero quantum with a nonzero usage count remains invalid. */
    used = 1u;
    quantum = 0u;
    if (scheduler_tick_charge(&used, &priority, &quantum, 1u) == 0 ||
        used != 1u || priority != 1u || quantum != 0u) return 35;

    if (scheduler_tick_accumulate(0) != -1) return 36;
    pending_ticks = 0u;
    if (scheduler_tick_accumulate(&pending_ticks) != 1 ||
        pending_ticks != 1u) return 37;
    pending_ticks = UINT32_MAX;
    if (scheduler_tick_accumulate(&pending_ticks) != 0 ||
        pending_ticks != UINT32_MAX) return 38;

    uint32_t total_successes = 0u;
    pending_ticks = 0u;
    if (run_accumulator_workers(10000u, &total_successes) != 0 ||
        total_successes != 80000u || pending_ticks != 80000u) return 39;

    pending_ticks = UINT32_MAX - 100u;
    if (run_accumulator_workers(1000u, &total_successes) != 0 ||
        total_successes != 100u || pending_ticks != UINT32_MAX) return 40;

    if (deferred_tick_charge_after_contention_test() != 0) return 55;
    if (concurrent_tick_drain_test(10000u, 0u, 0) != 0) return 56;
    if (concurrent_tick_drain_test(1000u, UINT32_MAX - 64u, 1) != 0)
        return 57;
    return 0;
}
