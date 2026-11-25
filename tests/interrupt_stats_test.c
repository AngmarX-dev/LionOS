#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include "interrupt_stats.h"

#define WORKERS_PER_CPU 4u
#define WORKER_COUNT (LIONOS_INTERRUPT_STATS_CPUS * WORKERS_PER_CPU)
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

    /*
     * Exercise every configured CPU slot, including the highest valid index.
     * Two writers contend on one vector per CPU; independent writers update a
     * second vector and vector 200. The latter is shared by number across all
     * CPUs, checking that the CPU dimension remains isolated.
     */
    uint32_t created = 0u;
    for (uint32_t i = 0u; i < WORKER_COUNT; ++i) {
        uint32_t cpu = i / WORKERS_PER_CPU;
        uint32_t lane = i % WORKERS_PER_CPU;
        args[i].cpu = cpu;
        if (lane < 2u)
            args[i].vector = 64u + cpu;
        else if (lane == 2u)
            args[i].vector = 160u + cpu;
        else
            args[i].vector = 200u;

        if (pthread_create(&threads[i], 0, record_worker, &args[i]) != 0) {
            fputs("interrupt stats: pthread_create failed\n", stderr);
            for (uint32_t joined = 0u; joined < created; ++joined)
                (void)pthread_join(threads[joined], 0);
            return 1;
        }
        ++created;
    }

    for (uint32_t i = 0u; i < WORKER_COUNT; ++i) {
        if (pthread_join(threads[i], 0) != 0) {
            fputs("interrupt stats: pthread_join failed\n", stderr);
            return 2;
        }
    }

    /* Check shared counters only after every writer has stopped. */
    for (uint32_t cpu = 0u; cpu < LIONOS_INTERRUPT_STATS_CPUS; ++cpu) {
        if (interrupt_stats_count(cpu, 64u + cpu) != (2u * ITERATIONS) ||
            interrupt_stats_count(cpu, 160u + cpu) != ITERATIONS ||
            interrupt_stats_count(cpu, 200u) != ITERATIONS) {
            fputs("interrupt stats: concurrent vector count mismatch\n",
                  stderr);
            return 3;
        }

        if (interrupt_stats_total(cpu) != (4u * ITERATIONS)) {
            fputs("interrupt stats: per-CPU total mismatch\n", stderr);
            return 4;
        }
    }

    /* A different vector on one CPU must remain independently countable. */
    interrupt_stats_record(0u, 128u);
    if (interrupt_stats_count(0u, 128u) != 1u ||
        interrupt_stats_total(0u) != (4u * ITERATIONS + 1u) ||
        interrupt_stats_count(1u, 128u) != 0u) {
        fputs("interrupt stats: vector isolation failed\n", stderr);
        return 7;
    }

    /*
     * Exercise the final valid CPU/vector cell, then issue invalid writes
     * immediately beyond each dimension. The valid boundary cell and an
     * unrelated CPU's matching vector must remain unchanged.
     */
    const uint32_t last_cpu = LIONOS_INTERRUPT_STATS_CPUS - 1u;
    const uint32_t last_vector = LIONOS_INTERRUPT_STATS_VECTORS - 1u;
    if (interrupt_stats_count(last_cpu, last_vector) != 0u ||
        interrupt_stats_count(0u, last_vector) != 0u) {
        fputs("interrupt stats: boundary slot was not initially clear\n",
              stderr);
        return 8;
    }
    interrupt_stats_record(last_cpu, last_vector);
    interrupt_stats_record(last_cpu, last_vector);
    interrupt_stats_record(last_cpu, last_vector);

    /* Invalid CPU/vector indices must not access the counter array. */
    interrupt_stats_record(LIONOS_INTERRUPT_STATS_CPUS, last_vector);
    interrupt_stats_record(last_cpu, LIONOS_INTERRUPT_STATS_VECTORS);
    if (interrupt_stats_count(last_cpu, last_vector) != 3u ||
        interrupt_stats_count(0u, last_vector) != 0u ||
        interrupt_stats_count(LIONOS_INTERRUPT_STATS_CPUS, last_vector) != 0u ||
        interrupt_stats_count(last_cpu, LIONOS_INTERRUPT_STATS_VECTORS) != 0u ||
        interrupt_stats_total(LIONOS_INTERRUPT_STATS_CPUS) != 0u) {
        fputs("interrupt stats: boundary/bounds validation failed\n", stderr);
        return 5;
    }

    puts("per-CPU interrupt statistics tests passed");
    return 0;
}
