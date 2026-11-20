#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include "interrupt_stats.h"

#define WORKER_COUNT 8u
#define ITERATIONS 50000u

struct worker_args {
    uint32_t cpu;
    uint32_t vector;
};

static void *record_worker(void *opaque)
{
    const struct worker_args *args = (const struct worker_args *)opaque;
    for (uint32_t i = 0u; i < ITERATIONS; ++i)
        interrupt_stats_record(args->cpu, args->vector);
    return 0;
}

int main(void)
{
    pthread_t threads[WORKER_COUNT];
    struct worker_args args[WORKER_COUNT];

    for (uint32_t i = 0u; i < WORKER_COUNT; ++i) {
        args[i].cpu = i % 4u;
        args[i].vector = 64u + args[i].cpu;
        if (pthread_create(&threads[i], 0, record_worker, &args[i]) != 0) {
            fputs("interrupt stats: pthread_create failed\n", stderr);
            return 1;
        }
    }

    for (uint32_t i = 0u; i < WORKER_COUNT; ++i) {
        if (pthread_join(threads[i], 0) != 0) {
            fputs("interrupt stats: pthread_join failed\n", stderr);
            return 2;
        }
        if (interrupt_stats_count(args[i].cpu, args[i].vector) !=
            (2u * ITERATIONS)) {
            fputs("interrupt stats: concurrent count mismatch\n", stderr);
            return 3;
        }
    }

    for (uint32_t cpu = 0u; cpu < 4u; ++cpu) {
        uint32_t expected = 0u;
        for (uint32_t i = 0u; i < WORKER_COUNT; ++i)
            if (args[i].cpu == cpu)
                expected += ITERATIONS;
        if (interrupt_stats_total(cpu) != expected) {
            fputs("interrupt stats: per-CPU total mismatch\n", stderr);
            return 4;
        }
    }

    /* A different vector on the same CPU must remain independently countable. */
    interrupt_stats_record(0u, 128u);
    if (interrupt_stats_count(0u, 128u) != 1u ||
        interrupt_stats_total(0u) != (2u * ITERATIONS + 1u)) {
        fputs("interrupt stats: vector isolation failed\\n", stderr);
        return 7;
    }

    /* Invalid CPU/vector indices must not access the counter array. */
    interrupt_stats_record(LIONOS_INTERRUPT_STATS_CPUS, 1u);
    interrupt_stats_record(0u, LIONOS_INTERRUPT_STATS_VECTORS);
    if (interrupt_stats_count(LIONOS_INTERRUPT_STATS_CPUS, 1u) != 0u ||
        interrupt_stats_count(0u, LIONOS_INTERRUPT_STATS_VECTORS) != 0u ||
        interrupt_stats_total(LIONOS_INTERRUPT_STATS_CPUS) != 0u) {
        fputs("interrupt stats: bounds validation failed\n", stderr);
        return 5;
    }

    if (interrupt_stats_count(4u, 64u) != 0u ||
        interrupt_stats_count(5u, 65u) != 0u) {
        fputs("interrupt stats: CPU counter isolation failed\n", stderr);
        return 6;
    }

    puts("per-CPU interrupt statistics tests passed");
    return 0;
}
