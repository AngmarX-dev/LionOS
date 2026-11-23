#include <pthread.h>
#include <stdint.h>
#include "runqueue.h"
#include "scheduler_policy.h"

#define TEST_CPU_COUNT 4u
#define WORKERS_PER_CPU 2u
#define WORKER_COUNT (TEST_CPU_COUNT * WORKERS_PER_CPU)
#define NODES_PER_WORKER 8u
#define NODE_COUNT (WORKER_COUNT * NODES_PER_WORKER)
#define ITERATIONS 20000u
#define TRANSFER_ITERATIONS 10000u
#define PRIORITY_ITERATIONS 20000u

struct worker_argument {
    uint32_t cpu;
    uint32_t worker;
};

struct transfer_argument {
    uint32_t worker;
    volatile uint32_t *start;
};

struct priority_argument {
    uint32_t worker;
    volatile uint32_t *start;
};

static struct runqueue queues[TEST_CPU_COUNT];
static struct runqueue_node nodes[NODE_COUNT];
static struct worker_argument arguments[WORKER_COUNT];
static struct transfer_argument transfer_arguments[WORKER_COUNT];
static volatile uint32_t failed;
static volatile uint32_t transfer_first_snapshot_ready;
static volatile uint32_t transfer_first_snapshot_expected;
static volatile uint32_t transfer_stale_owner_retries;

void spinlock_init(struct spinlock *lock) {
    if (lock) __atomic_store_n(&lock->value, 0u, __ATOMIC_RELAXED);
}

uint32_t spinlock_try_acquire(struct spinlock *lock) {
    uint32_t expected = 0u;
    return lock && __atomic_compare_exchange_n(&lock->value, &expected, 1u,
        0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void spinlock_acquire(struct spinlock *lock) {
    while (!spinlock_try_acquire(lock))
        __asm__ volatile("pause");
}

void spinlock_release(struct spinlock *lock) {
    if (lock) __atomic_store_n(&lock->value, 0u, __ATOMIC_RELEASE);
}

static void fail_test(void) {
    __atomic_store_n(&failed, 1u, __ATOMIC_RELEASE);
}

static void lock_queue_pair(uint32_t a, uint32_t b) {
    uint32_t first = a < b ? a : b;
    uint32_t second = a < b ? b : a;
    spinlock_acquire(&queues[first].lock);
    spinlock_acquire(&queues[second].lock);
}

static void unlock_queue_pair(uint32_t a, uint32_t b) {
    uint32_t first = a < b ? a : b;
    uint32_t second = a < b ? b : a;
    spinlock_release(&queues[second].lock);
    spinlock_release(&queues[first].lock);
}

/*
 * queue_cpu is protected by the owning queue lock. Lock every queue in
 * ascending CPU order to obtain a stable owner snapshot while transfers may
 * move this node between queues.
 */
static uint32_t node_owner_snapshot(uint32_t index) {
    for (uint32_t cpu = 0u; cpu < TEST_CPU_COUNT; ++cpu)
        spinlock_acquire(&queues[cpu].lock);

    uint32_t owner = nodes[index].queue_cpu;

    for (uint32_t cpu = TEST_CPU_COUNT; cpu > 0u; --cpu)
        spinlock_release(&queues[cpu - 1u].lock);
    return owner;
}

static void *run_worker(void *opaque) {
    const struct worker_argument *argument =
        (const struct worker_argument *)opaque;
    struct runqueue *queue = &queues[argument->cpu];
    uint32_t first = argument->worker * NODES_PER_WORKER;

    for (uint32_t round = 0u; round < ITERATIONS; ++round) {
        spinlock_acquire(&queue->lock);
        for (uint32_t i = 0u; i < NODES_PER_WORKER; ++i) {
            uint32_t index = first + i;
            uint32_t priority = (round + i + argument->worker) %
                               LIONOS_RUNQUEUE_PRIORITY_LEVELS;
            if (runqueue_enqueue_locked(queue, nodes, NODE_COUNT,
                                        index, priority) != 0 ||
                runqueue_validate_locked(queue, nodes, NODE_COUNT, 0) != 0) {
                fail_test();
                spinlock_release(&queue->lock);
                return 0;
            }
        }

        for (uint32_t i = 0u; i < NODES_PER_WORKER; ++i) {
            if (runqueue_dequeue_locked(queue, nodes, NODE_COUNT,
                                        first + i) != 0 ||
                runqueue_validate_locked(queue, nodes, NODE_COUNT, 0) != 0) {
                fail_test();
                spinlock_release(&queue->lock);
                return 0;
            }
        }
        spinlock_release(&queue->lock);
    }
    return 0;
}

static uint32_t transfer_random(uint32_t *state) {
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static void *run_transfer_worker(void *opaque) {
    const struct transfer_argument *argument =
        (const struct transfer_argument *)opaque;
    uint32_t random_state =
        0x9E3779B9u ^ ((argument->worker + 1u) * 0x85EBCA6Bu);

    while (__atomic_load_n(argument->start, __ATOMIC_ACQUIRE) == 0u)
        __asm__ volatile("pause");

    /*
     * Every transfer worker chooses from the complete node set. Unlike a
     * disjoint per-worker partition, this makes two transfer workers able
     * to race for the same node and exercises stale-owner retry in practice.
     * Keep the total operation count equal to the previous nested workload.
     */
    for (uint32_t operation = 0u;
         operation < TRANSFER_ITERATIONS * NODES_PER_WORKER;
         ++operation) {
        uint32_t index = operation == 0u
            ? 0u : transfer_random(&random_state) % NODE_COUNT;
        uint32_t retries = 0u;

        for (;;) {
            uint32_t owner = node_owner_snapshot(index);
            if (owner >= TEST_CPU_COUNT) {
                fail_test();
                return 0;
            }

            /*
             * Force all transfer workers to snapshot the same node before
             * any of them can acquire its source/destination pair. The first
             * move then makes every other initial snapshot stale.
             */
            if (operation == 0u && retries == 0u) {
                __atomic_fetch_add(&transfer_first_snapshot_ready, 1u,
                                   __ATOMIC_ACQ_REL);
                while (__atomic_load_n(&transfer_first_snapshot_ready,
                                       __ATOMIC_ACQUIRE) !=
                       __atomic_load_n(&transfer_first_snapshot_expected,
                                       __ATOMIC_ACQUIRE))
                    __asm__ volatile("pause");
            }

            uint32_t destination_cpu;
            if (operation == 0u && retries == 0u) {
                destination_cpu = (owner + 1u) % TEST_CPU_COUNT;
            } else {
                destination_cpu =
                    transfer_random(&random_state) % TEST_CPU_COUNT;
                if (destination_cpu == owner)
                    destination_cpu = (destination_cpu + 1u) % TEST_CPU_COUNT;
            }

            lock_queue_pair(owner, destination_cpu);
            /*
             * A competing transfer may have changed the owner after the
             * stable snapshot. Retry this same transfer operation rather
             * than silently dropping it from the stress workload.
             */
            if (nodes[index].queue_cpu != owner) {
                unlock_queue_pair(owner, destination_cpu);
                __atomic_fetch_add(&transfer_stale_owner_retries, 1u,
                                   __ATOMIC_RELAXED);
                if (++retries >= 65536u) {
                    fail_test();
                    return 0;
                }
                continue;
            }

            if (runqueue_move_locked(&queues[owner],
                    &queues[destination_cpu], nodes, NODE_COUNT, index) != 0 ||
                runqueue_validate_locked(&queues[owner], nodes,
                    NODE_COUNT, 0) != 0 ||
                runqueue_validate_locked(&queues[destination_cpu], nodes,
                    NODE_COUNT, 0) != 0) {
                fail_test();
                unlock_queue_pair(owner, destination_cpu);
                return 0;
            }
            unlock_queue_pair(owner, destination_cpu);
            break;
        }
    }
    return 0;
}


static void *run_priority_worker(void *opaque) {
    const struct priority_argument *argument =
        (const struct priority_argument *)opaque;
    uint32_t first = argument->worker * NODES_PER_WORKER;

    while (__atomic_load_n(argument->start, __ATOMIC_ACQUIRE) == 0u)
        __asm__ volatile("pause");

    for (uint32_t round = 0u; round < PRIORITY_ITERATIONS; ++round) {
        for (uint32_t i = 0u; i < NODES_PER_WORKER; ++i) {
            uint32_t index = first + i;
            uint32_t retries = 0u;
            for (;;) {
                /* Transfers may change queue_cpu concurrently. Take a
                 * stable snapshot, then revalidate under the owner lock. */
                uint32_t owner = node_owner_snapshot(index);
                if (owner >= TEST_CPU_COUNT) {
                    fail_test();
                    return 0;
                }

                struct runqueue *queue = &queues[owner];
                spinlock_acquire(&queue->lock);
                if (nodes[index].queue_cpu != owner) {
                    spinlock_release(&queue->lock);
                    if (++retries >= 65536u) {
                        fail_test();
                        return 0;
                    }
                    continue;
                }
                if (runqueue_requeue_priority_locked(queue, nodes,
                        NODE_COUNT, index,
                        (round + i + argument->worker) %
                        LIONOS_RUNQUEUE_PRIORITY_LEVELS) != 0 ||
                    runqueue_validate_locked(queue, nodes,
                        NODE_COUNT, 0) != 0) {
                    fail_test();
                    spinlock_release(&queue->lock);
                    return 0;
                }
                spinlock_release(&queue->lock);
                break;
            }
        }
    }
    return 0;
}

#define BUSY_RACE_NODE_COUNT 8u

struct busy_race_argument {
    uint32_t destination_cpu;
    volatile uint32_t *start;
    int result;
};

static struct runqueue busy_race_queues[3];
static struct runqueue_node busy_race_nodes[BUSY_RACE_NODE_COUNT];

static int busy_race_try_transfer(uint32_t destination_cpu) {
    const uint32_t source_cpu = 0u;
    if (destination_cpu == source_cpu || destination_cpu >= 3u)
        return -1;
    struct runqueue *source = &busy_race_queues[source_cpu];
    struct runqueue *destination = &busy_race_queues[destination_cpu];
    uint32_t first = source_cpu < destination_cpu
        ? source_cpu : destination_cpu;
    uint32_t second = source_cpu < destination_cpu
        ? destination_cpu : source_cpu;

    if (!spinlock_try_acquire(&busy_race_queues[first].lock))
        return 0;
    if (!spinlock_try_acquire(&busy_race_queues[second].lock)) {
        spinlock_release(&busy_race_queues[first].lock);
        return 0;
    }

    int result = 0;
    if (runqueue_validate_locked(source, busy_race_nodes,
            BUSY_RACE_NODE_COUNT, 0) != 0 ||
        runqueue_validate_locked(destination, busy_race_nodes,
            BUSY_RACE_NODE_COUNT, 0) != 0) {
        result = -1;
    } else if (runqueue_should_balance(destination->total,
                                        source->total, 0)) {
        uint32_t candidate = LIONOS_RUNQUEUE_INVALID_INDEX;
        for (uint32_t priority = 0u;
             priority < LIONOS_RUNQUEUE_PRIORITY_LEVELS;
             ++priority) {
            candidate = source->head[priority];
            if (candidate != LIONOS_RUNQUEUE_INVALID_INDEX)
                break;
        }
        if (candidate == LIONOS_RUNQUEUE_INVALID_INDEX ||
            runqueue_move_locked(source, destination, busy_race_nodes,
                BUSY_RACE_NODE_COUNT, candidate) != 0)
            result = -1;
        else
            result = 1;
    }

    spinlock_release(&busy_race_queues[second].lock);
    spinlock_release(&busy_race_queues[first].lock);
    return result;
}

static void *run_busy_balance_racer(void *opaque) {
    struct busy_race_argument *argument =
        (struct busy_race_argument *)opaque;
    while (__atomic_load_n(argument->start, __ATOMIC_ACQUIRE) == 0u)
        __asm__ volatile("pause");

    argument->result = 0;
    for (uint32_t attempt = 0u; attempt < 10000000u; ++attempt) {
        int result = busy_race_try_transfer(argument->destination_cpu);
        if (result != 0) {
            argument->result = result;
            return 0;
        }
        __asm__ volatile("pause");
    }
    return 0;
}

static int busy_race_enqueue(uint32_t cpu, uint32_t index) {
    struct runqueue *queue = &busy_race_queues[cpu];
    spinlock_acquire(&queue->lock);
    int result = runqueue_enqueue_locked(queue, busy_race_nodes,
        BUSY_RACE_NODE_COUNT, index, 2u);
    spinlock_release(&queue->lock);
    return result;
}

static int busy_race_pick(uint32_t cpu, uint32_t *picked) {
    struct runqueue *queue = &busy_race_queues[cpu];
    spinlock_acquire(&queue->lock);
    int result = runqueue_pick_next_locked(queue, busy_race_nodes,
        BUSY_RACE_NODE_COUNT, picked);
    spinlock_release(&queue->lock);
    return result;
}

static int busy_race_validate(uint32_t cpu, uint32_t *total) {
    struct runqueue *queue = &busy_race_queues[cpu];
    spinlock_acquire(&queue->lock);
    int result = runqueue_validate_locked(queue, busy_race_nodes,
        BUSY_RACE_NODE_COUNT, total);
    spinlock_release(&queue->lock);
    return result;
}

#define SHARED_TRANSFER_ITERATIONS 10000u
#define SHARED_TRANSFER_MAX_ATTEMPTS (SHARED_TRANSFER_ITERATIONS * 64u)

struct shared_transfer_argument {
    uint32_t worker;
    uint32_t completed;
};

static void *run_shared_transfer_worker(void *opaque) {
    struct shared_transfer_argument *argument =
        (struct shared_transfer_argument *)opaque;

    uint32_t attempts = 0u;
    argument->completed = 0u;
    while (argument->completed < SHARED_TRANSFER_ITERATIONS &&
           attempts < SHARED_TRANSFER_MAX_ATTEMPTS) {
        ++attempts;
        const uint32_t index = 0u;
        uint32_t owner = node_owner_snapshot(index);
        uint32_t destination =
            (argument->worker + argument->completed) % TEST_CPU_COUNT;

        if (owner >= TEST_CPU_COUNT) {
            fail_test();
            return 0;
        }
        if (destination == owner)
            destination = (destination + 1u) % TEST_CPU_COUNT;

        lock_queue_pair(owner, destination);
        /*
         * The other worker may have won the same-node transfer race after
         * our snapshot. Retry with a fresh owner rather than moving from a
         * stale source queue.
         */
        if (nodes[index].queue_cpu != owner) {
            unlock_queue_pair(owner, destination);
            continue;
        }
        if (runqueue_move_locked(&queues[owner], &queues[destination],
                nodes, NODE_COUNT, index) != 0 ||
            runqueue_validate_locked(&queues[owner], nodes,
                NODE_COUNT, 0) != 0 ||
            runqueue_validate_locked(&queues[destination], nodes,
                NODE_COUNT, 0) != 0) {
            fail_test();
            unlock_queue_pair(owner, destination);
            return 0;
        }
        unlock_queue_pair(owner, destination);
        ++argument->completed;
    }
    if (argument->completed != SHARED_TRANSFER_ITERATIONS)
        fail_test();
    return 0;
}

static int shared_node_transfer_race_test(void) {
    pthread_t threads[2];
    struct shared_transfer_argument arguments[2] = {{0u, 0u}, {1u, 0u}};
    uint32_t created = 0u;
    uint32_t total = 0u;

    for (uint32_t cpu = 0u; cpu < TEST_CPU_COUNT; ++cpu)
        runqueue_init(&queues[cpu], cpu);
    for (uint32_t i = 0u; i < NODE_COUNT; ++i)
        runqueue_node_init(&nodes[i]);

    spinlock_acquire(&queues[0].lock);
    int enqueue_result = runqueue_enqueue_locked(&queues[0], nodes,
        NODE_COUNT, 0u, 1u);
    spinlock_release(&queues[0].lock);
    if (enqueue_result != 0)
        return 1;

    for (uint32_t i = 0u; i < 2u; ++i) {
        if (pthread_create(&threads[i], 0, run_shared_transfer_worker,
                           &arguments[i]) != 0)
            break;
        ++created;
    }
    for (uint32_t i = 0u; i < created; ++i)
        if (pthread_join(threads[i], 0) != 0)
            fail_test();
    if (created != 2u ||
        arguments[0].completed != SHARED_TRANSFER_ITERATIONS ||
        arguments[1].completed != SHARED_TRANSFER_ITERATIONS ||
        __atomic_load_n(&failed, __ATOMIC_ACQUIRE) != 0u)
        return 2;

    for (uint32_t cpu = 0u; cpu < TEST_CPU_COUNT; ++cpu)
        spinlock_acquire(&queues[cpu].lock);
    uint32_t global_total = 0u;
    uint32_t memberships = 0u;
    uint32_t owner = nodes[0].queue_cpu;
    for (uint32_t cpu = 0u; cpu < TEST_CPU_COUNT; ++cpu) {
        if (runqueue_validate_locked(&queues[cpu], nodes,
                NODE_COUNT, &total) != 0 ||
            total != queues[cpu].total ||
            total > NODE_COUNT - global_total) {
            fail_test();
            break;
        }
        global_total += total;
        for (uint32_t priority = 0u;
             priority < LIONOS_RUNQUEUE_PRIORITY_LEVELS; ++priority) {
            uint32_t index = queues[cpu].head[priority];
            while (index != LIONOS_RUNQUEUE_INVALID_INDEX) {
                if (index >= NODE_COUNT) {
                    fail_test();
                    break;
                }
                if (index == 0u)
                    ++memberships;
                index = nodes[index].next;
            }
        }
    }
    if (global_total != 1u || memberships != 1u ||
        owner >= TEST_CPU_COUNT || queues[owner].total != 1u ||
        nodes[0].queued != 1u)
        fail_test();

    for (uint32_t cpu = TEST_CPU_COUNT; cpu > 0u; --cpu)
        spinlock_release(&queues[cpu - 1u].lock);

    return __atomic_load_n(&failed, __ATOMIC_ACQUIRE) == 0u ? 0 : 3;
}

static int simultaneous_busy_destination_test(void) {
    pthread_t threads[2];
    struct busy_race_argument arguments[2] = {
        {1u, 0, 0}, {2u, 0, 0}
    };
    volatile uint32_t start = 0u;
    uint32_t created = 0u;
    uint32_t picked = LIONOS_RUNQUEUE_INVALID_INDEX;

    for (uint32_t cpu = 0u; cpu < 3u; ++cpu)
        runqueue_init(&busy_race_queues[cpu], cpu);
    for (uint32_t i = 0u; i < BUSY_RACE_NODE_COUNT; ++i)
        runqueue_node_init(&busy_race_nodes[i]);

    for (uint32_t i = 0u; i < 4u; ++i)
        if (busy_race_enqueue(0u, i) != 0)
            return 1;
    if (busy_race_enqueue(1u, 4u) != 0 ||
        busy_race_enqueue(2u, 5u) != 0)
        return 2;

    /* Each destination has a running task selected outside its READY queue. */
    for (uint32_t cpu = 1u; cpu <= 2u; ++cpu) {
        if (busy_race_pick(cpu, &picked) != 0 || picked != cpu + 3u)
            return 3;
    }

    /* A held source lock must make a nonblocking stealer defer immediately. */
    spinlock_acquire(&busy_race_queues[0].lock);
    int deferred = busy_race_try_transfer(1u);
    spinlock_release(&busy_race_queues[0].lock);
    if (deferred != 0)
        return 4;

    for (uint32_t i = 0u; i < 2u; ++i) {
        arguments[i].start = &start;
        if (pthread_create(&threads[i], 0, run_busy_balance_racer,
                           &arguments[i]) != 0)
            break;
        ++created;
    }
    __atomic_store_n(&start, 1u, __ATOMIC_RELEASE);
    for (uint32_t i = 0u; i < created; ++i)
        if (pthread_join(threads[i], 0) != 0)
            return 5;
    if (created != 2u || arguments[0].result != 1 ||
        arguments[1].result != 1)
        return 6;

    uint32_t total = 0u;
    if (busy_race_queues[0].total != 2u ||
        busy_race_queues[1].total != 1u ||
        busy_race_queues[2].total != 1u ||
        busy_race_validate(0u, &total) != 0 || total != 2u ||
        busy_race_validate(1u, &total) != 0 || total != 1u ||
        busy_race_validate(2u, &total) != 0 || total != 1u)
        return 7;

    for (uint32_t i = 0u; i < BUSY_RACE_NODE_COUNT; ++i) {
        uint32_t memberships = 0u;
        uint32_t membership_cpu = LIONOS_RUNQUEUE_INVALID_INDEX;
        for (uint32_t cpu = 0u; cpu < 3u; ++cpu) {
            for (uint32_t priority = 0u;
                 priority < LIONOS_RUNQUEUE_PRIORITY_LEVELS; ++priority) {
                uint32_t index = busy_race_queues[cpu].head[priority];
                uint32_t visited = 0u;
                while (index != LIONOS_RUNQUEUE_INVALID_INDEX) {
                    if (index >= BUSY_RACE_NODE_COUNT ||
                        visited++ >= BUSY_RACE_NODE_COUNT)
                        return 10;
                    if (index == i) {
                        ++memberships;
                        membership_cpu = cpu;
                    }
                    index = busy_race_nodes[index].next;
                }
            }
        }
        if (i < 4u) {
            if (memberships != 1u ||
                busy_race_nodes[i].queued != 1u ||
                busy_race_nodes[i].queue_cpu != membership_cpu)
                return 8;
        } else if (memberships != 0u ||
                   busy_race_nodes[i].queued != 0u ||
                   busy_race_nodes[i].queue_cpu !=
                       LIONOS_RUNQUEUE_INVALID_INDEX ||
                   busy_race_nodes[i].next != LIONOS_RUNQUEUE_INVALID_INDEX ||
                   busy_race_nodes[i].previous !=
                       LIONOS_RUNQUEUE_INVALID_INDEX) {
            return 9;
        }
    }
    if (busy_race_queues[1].total + busy_race_queues[2].total != 2u)
        return 11;
    return 0;
}


/*
 * Concurrent production of READY nodes, local dispatch, and bounded
 * busy-destination transfers. This validates run-queue mechanics under
 * overlapping host threads; process_lock and production scheduler execution
 * are intentionally outside this host-side test.
 */
#define SUSTAINED_CPU_COUNT 3u
#define SUSTAINED_INITIAL_BURST 96u
#define SUSTAINED_ARRIVAL_ROUNDS 2048u
#define SUSTAINED_NODES (SUSTAINED_INITIAL_BURST + 3u * SUSTAINED_ARRIVAL_ROUNDS)

struct sustained_test_state {
    struct runqueue queues[SUSTAINED_CPU_COUNT];
    struct runqueue_node nodes[SUSTAINED_NODES];
    uint8_t completed_seen[SUSTAINED_NODES];
    volatile uint32_t producer_done;
    volatile uint32_t balancer_done;
    volatile uint32_t service_ready_mask;
    volatile uint32_t service_go;
    volatile uint32_t abort;
    volatile uint32_t failed;
    volatile uint32_t completed;
    volatile uint32_t transfers;
    volatile uint32_t destination_mask;
    uint32_t balance_cursor[SUSTAINED_CPU_COUNT];
};

struct sustained_worker_argument {
    struct sustained_test_state *state;
    uint32_t cpu;
};

static struct sustained_test_state sustained_state;

static int sustained_lock_pair(struct sustained_test_state *state,
                               uint32_t a, uint32_t b) {
    uint32_t first = a < b ? a : b;
    uint32_t second = a < b ? b : a;
    if (!spinlock_try_acquire(&state->queues[first].lock))
        return 0;
    if (!spinlock_try_acquire(&state->queues[second].lock)) {
        spinlock_release(&state->queues[first].lock);
        return 0;
    }
    return 1;
}

static void sustained_unlock_pair(struct sustained_test_state *state,
                                  uint32_t a, uint32_t b) {
    uint32_t first = a < b ? a : b;
    uint32_t second = a < b ? b : a;
    spinlock_release(&state->queues[second].lock);
    spinlock_release(&state->queues[first].lock);
}

static void *sustained_producer(void *opaque) {
    struct sustained_test_state *state =
        (struct sustained_test_state *)opaque;
    struct runqueue *source = &state->queues[0];

    while (__atomic_load_n(&state->service_go, __ATOMIC_ACQUIRE) == 0u &&
           __atomic_load_n(&state->abort, __ATOMIC_ACQUIRE) == 0u)
        __asm__ volatile("pause");
    for (uint32_t round = 0u; round < SUSTAINED_ARRIVAL_ROUNDS; ++round) {
        if (__atomic_load_n(&state->abort, __ATOMIC_ACQUIRE) != 0u)
            break;
        spinlock_acquire(&source->lock);
        for (uint32_t item = 0u; item < 3u; ++item) {
            uint32_t index = SUSTAINED_INITIAL_BURST + round * 3u + item;
            if (runqueue_enqueue_locked(source, state->nodes,
                    SUSTAINED_NODES, index,
                    (round + item) % LIONOS_RUNQUEUE_PRIORITY_LEVELS) != 0) {
                __atomic_store_n(&state->failed, 1u, __ATOMIC_RELEASE);
                break;
            }
        }
        spinlock_release(&source->lock);
        if (__atomic_load_n(&state->failed, __ATOMIC_ACQUIRE) != 0u)
            break;
    }
    __atomic_store_n(&state->producer_done, 1u, __ATOMIC_RELEASE);
    return 0;
}

static void sustained_queue_totals_snapshot(
    struct sustained_test_state *state,
    uint32_t totals[SUSTAINED_CPU_COUNT]) {
    /* Acquire all queue locks in CPU order for one coherent policy snapshot. */
    for (uint32_t cpu = 0u; cpu < SUSTAINED_CPU_COUNT; ++cpu)
        spinlock_acquire(&state->queues[cpu].lock);
    for (uint32_t cpu = 0u; cpu < SUSTAINED_CPU_COUNT; ++cpu)
        totals[cpu] = state->queues[cpu].total;
    for (uint32_t cpu = SUSTAINED_CPU_COUNT; cpu > 0u; --cpu)
        spinlock_release(&state->queues[cpu - 1u].lock);
}

static void *sustained_balancer(void *opaque) {
    struct sustained_test_state *state =
        (struct sustained_test_state *)opaque;
    uint32_t attempts = 0u;

    while (__atomic_load_n(&state->service_go, __ATOMIC_ACQUIRE) == 0u &&
           __atomic_load_n(&state->abort, __ATOMIC_ACQUIRE) == 0u)
        __asm__ volatile("pause");
    while (__atomic_load_n(&state->abort, __ATOMIC_ACQUIRE) == 0u &&
           attempts++ < 100000000u) {
        int progress = 0;
        for (uint32_t destination = 1u;
             destination < SUSTAINED_CPU_COUNT; ++destination) {
            uint32_t totals[SUSTAINED_CPU_COUNT];
            sustained_queue_totals_snapshot(state, totals);
            uint32_t victim = scheduler_select_victim(
                totals, SUSTAINED_CPU_COUNT, destination,
                state->balance_cursor[destination], 0u);
            if (victim == UINT32_MAX ||
                !runqueue_should_balance(totals[destination], totals[victim], 0))
                continue;

            if (!sustained_lock_pair(state, victim, destination))
                continue;
            struct runqueue *source = &state->queues[victim];
            struct runqueue *target = &state->queues[destination];
            if (runqueue_validate_locked(source, state->nodes,
                    SUSTAINED_NODES, 0) != 0 ||
                runqueue_validate_locked(target, state->nodes,
                    SUSTAINED_NODES, 0) != 0) {
                __atomic_store_n(&state->failed, 1u, __ATOMIC_RELEASE);
                sustained_unlock_pair(state, victim, destination);
                __atomic_store_n(&state->abort, 1u, __ATOMIC_RELEASE);
                break;
            }
            /* The coherent snapshot may be stale after its locks are released. */
            if (runqueue_should_balance(target->total, source->total, 0)) {
                uint32_t candidate = LIONOS_RUNQUEUE_INVALID_INDEX;
                for (uint32_t priority = 0u;
                     priority < LIONOS_RUNQUEUE_PRIORITY_LEVELS; ++priority) {
                    candidate = source->head[priority];
                    if (candidate != LIONOS_RUNQUEUE_INVALID_INDEX)
                        break;
                }
                if (candidate == LIONOS_RUNQUEUE_INVALID_INDEX ||
                    runqueue_move_locked(source, target, state->nodes,
                        SUSTAINED_NODES, candidate) != 0) {
                    __atomic_store_n(&state->failed, 1u, __ATOMIC_RELEASE);
                    sustained_unlock_pair(state, victim, destination);
                    __atomic_store_n(&state->abort, 1u, __ATOMIC_RELEASE);
                    break;
                }
                state->balance_cursor[destination] =
                    (victim + 1u) % SUSTAINED_CPU_COUNT;
                __atomic_fetch_or(&state->destination_mask,
                    1u << destination, __ATOMIC_RELEASE);
                __atomic_fetch_add(&state->transfers, 1u, __ATOMIC_RELAXED);
                progress = 1;
            }
            sustained_unlock_pair(state, victim, destination);
        }

        if (__atomic_load_n(&state->abort, __ATOMIC_ACQUIRE) != 0u)
            break;

        if (__atomic_load_n(&state->producer_done, __ATOMIC_ACQUIRE) != 0u) {
            spinlock_acquire(&state->queues[0].lock);
            int source_empty = state->queues[0].total == 0u;
            spinlock_release(&state->queues[0].lock);
            if (source_empty) {
                __atomic_store_n(&state->balancer_done, 1u, __ATOMIC_RELEASE);
                return 0;
            }
        }
        if (!progress)
            __asm__ volatile("pause");
    }

    if (__atomic_load_n(&state->abort, __ATOMIC_ACQUIRE) == 0u) {
        __atomic_store_n(&state->failed, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&state->abort, 1u, __ATOMIC_RELEASE);
    }
    return 0;
}

static void *sustained_service_worker(void *opaque) {
    const struct sustained_worker_argument *argument =
        (const struct sustained_worker_argument *)opaque;
    struct sustained_test_state *state = argument->state;
    struct runqueue *queue = &state->queues[argument->cpu];

    __atomic_fetch_or(&state->service_ready_mask, 1u << argument->cpu,
                      __ATOMIC_RELEASE);
    while (__atomic_load_n(&state->service_go, __ATOMIC_ACQUIRE) == 0u &&
           __atomic_load_n(&state->abort, __ATOMIC_ACQUIRE) == 0u)
        __asm__ volatile("pause");

    /*
     * Keep every simulated CPU dispatch-ready but do not let the source CPU
     * consume the initial burst before both busy destinations have accepted
     * work. This makes the distribution assertion independent of host thread
     * scheduling while leaving the balancer and queue policy under contention.
     */
    const uint32_t required_destinations = (1u << 1u) | (1u << 2u);
    while ((__atomic_load_n(&state->destination_mask, __ATOMIC_ACQUIRE) &
            required_destinations) != required_destinations &&
           __atomic_load_n(&state->abort, __ATOMIC_ACQUIRE) == 0u)
        __asm__ volatile("pause");

    while (__atomic_load_n(&state->abort, __ATOMIC_ACQUIRE) == 0u) {
        uint32_t index = LIONOS_RUNQUEUE_INVALID_INDEX;
        spinlock_acquire(&queue->lock);
        int picked = runqueue_pick_next_locked(queue, state->nodes,
                                               SUSTAINED_NODES, &index);
        spinlock_release(&queue->lock);
        if (picked < 0) {
            __atomic_store_n(&state->failed, 1u, __ATOMIC_RELEASE);
            __atomic_store_n(&state->abort, 1u, __ATOMIC_RELEASE);
            return 0;
        }
        if (picked == 0) {
            if (index >= SUSTAINED_NODES) {
                __atomic_store_n(&state->failed, 1u, __ATOMIC_RELEASE);
                __atomic_store_n(&state->abort, 1u, __ATOMIC_RELEASE);
                return 0;
            }
            uint8_t expected = 0u;
            if (!__atomic_compare_exchange_n(&state->completed_seen[index],
                    &expected, 1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&state->failed, 1u, __ATOMIC_RELEASE);
                __atomic_store_n(&state->abort, 1u, __ATOMIC_RELEASE);
                return 0;
            }
            __atomic_fetch_add(&state->completed, 1u, __ATOMIC_RELAXED);
            continue;
        }

        if (__atomic_load_n(&state->producer_done, __ATOMIC_ACQUIRE) != 0u &&
            __atomic_load_n(&state->balancer_done, __ATOMIC_ACQUIRE) != 0u)
            return 0;
        __asm__ volatile("pause");
    }
    return 0;
}

static int sustained_concurrent_arrival_test(void) {
    pthread_t balancer_thread;
    pthread_t producer_thread;
    pthread_t service_threads[SUSTAINED_CPU_COUNT];
    struct sustained_worker_argument arguments[SUSTAINED_CPU_COUNT];
    struct sustained_test_state *state = &sustained_state;
    uint32_t service_created = 0u;
    uint32_t service_joined = 0u;
    int join_failed = 0;
    int balancer_created = 0;
    int producer_created = 0;

    for (uint32_t cpu = 0u; cpu < SUSTAINED_CPU_COUNT; ++cpu)
        runqueue_init(&state->queues[cpu], cpu);
    for (uint32_t i = 0u; i < SUSTAINED_NODES; ++i) {
        runqueue_node_init(&state->nodes[i]);
        __atomic_store_n(&state->completed_seen[i], 0u, __ATOMIC_RELAXED);
    }
    state->producer_done = 0u;
    state->balancer_done = 0u;
    state->service_ready_mask = 0u;
    state->service_go = 0u;
    state->abort = 0u;
    state->failed = 0u;
    state->completed = 0u;
    state->transfers = 0u;
    state->destination_mask = 0u;
    for (uint32_t cpu = 0u; cpu < SUSTAINED_CPU_COUNT; ++cpu)
        state->balance_cursor[cpu] = 0u;

    /* Preload a finite burst so both busy destinations must accept work. */
    spinlock_acquire(&state->queues[0].lock);
    for (uint32_t i = 0u; i < SUSTAINED_INITIAL_BURST; ++i) {
        if (runqueue_enqueue_locked(&state->queues[0], state->nodes,
                SUSTAINED_NODES, i,
                i % LIONOS_RUNQUEUE_PRIORITY_LEVELS) != 0) {
            spinlock_release(&state->queues[0].lock);
            return 1;
        }
    }
    spinlock_release(&state->queues[0].lock);

    for (uint32_t cpu = 0u; cpu < SUSTAINED_CPU_COUNT; ++cpu) {
        arguments[cpu].state = state;
        arguments[cpu].cpu = cpu;
        if (pthread_create(&service_threads[cpu], 0,
                sustained_service_worker, &arguments[cpu]) != 0)
            break;
        ++service_created;
    }
    if (service_created != SUSTAINED_CPU_COUNT) {
        __atomic_store_n(&state->abort, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&state->service_go, 1u, __ATOMIC_RELEASE);
        for (uint32_t cpu = 0u; cpu < service_created; ++cpu)
            if (pthread_join(service_threads[cpu], 0) != 0)
                join_failed = 1;
        return 3;
    }

    uint32_t ready_mask = (1u << SUSTAINED_CPU_COUNT) - 1u;
    uint32_t ready_wait = 0u;
    while (__atomic_load_n(&state->service_ready_mask, __ATOMIC_ACQUIRE) !=
               ready_mask && ready_wait++ < 10000000u)
        __asm__ volatile("pause");
    if (__atomic_load_n(&state->service_ready_mask, __ATOMIC_ACQUIRE) !=
        ready_mask) {
        __atomic_store_n(&state->abort, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&state->service_go, 1u, __ATOMIC_RELEASE);
        for (uint32_t cpu = 0u; cpu < service_created; ++cpu)
            if (pthread_join(service_threads[cpu], 0) != 0)
                join_failed = 1;
        return 3;
    }

    if (pthread_create(&balancer_thread, 0, sustained_balancer, state) == 0)
        balancer_created = 1;
    if (balancer_created &&
        pthread_create(&producer_thread, 0, sustained_producer, state) == 0)
        producer_created = 1;
    if (!balancer_created || !producer_created) {
        __atomic_store_n(&state->abort, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&state->producer_done, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&state->balancer_done, 1u, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&state->service_go, 1u, __ATOMIC_RELEASE);

    if (producer_created && pthread_join(producer_thread, 0) != 0)
        join_failed = 1;
    for (uint32_t cpu = 0u; cpu < service_created; ++cpu) {
        if (pthread_join(service_threads[cpu], 0) != 0)
            join_failed = 1;
        ++service_joined;
    }
    if (balancer_created && pthread_join(balancer_thread, 0) != 0)
        join_failed = 1;

    if (join_failed || !producer_created || service_created != SUSTAINED_CPU_COUNT ||
        service_joined != service_created ||
        __atomic_load_n(&state->failed, __ATOMIC_ACQUIRE) != 0u ||
        __atomic_load_n(&state->abort, __ATOMIC_ACQUIRE) != 0u ||
        __atomic_load_n(&state->producer_done, __ATOMIC_ACQUIRE) == 0u ||
        __atomic_load_n(&state->balancer_done, __ATOMIC_ACQUIRE) == 0u ||
        __atomic_load_n(&state->completed, __ATOMIC_ACQUIRE) != SUSTAINED_NODES ||
        __atomic_load_n(&state->transfers, __ATOMIC_ACQUIRE) < 2u ||
        __atomic_load_n(&state->destination_mask, __ATOMIC_ACQUIRE) !=
            ((1u << 1u) | (1u << 2u)))
        return 4;

    uint32_t global_total = 0u;
    for (uint32_t cpu = 0u; cpu < SUSTAINED_CPU_COUNT; ++cpu) {
        uint32_t total = 0u;
        spinlock_acquire(&state->queues[cpu].lock);
        int valid = runqueue_validate_locked(&state->queues[cpu], state->nodes,
                                              SUSTAINED_NODES, &total);
        spinlock_release(&state->queues[cpu].lock);
        if (valid != 0 || total != 0u)
            return 5;
        global_total += total;
    }
    if (global_total != 0u)
        return 6;
    for (uint32_t i = 0u; i < SUSTAINED_NODES; ++i)
        if (__atomic_load_n(&state->completed_seen[i], __ATOMIC_ACQUIRE) != 1u ||
            state->nodes[i].queued != 0u ||
            state->nodes[i].queue_cpu != LIONOS_RUNQUEUE_INVALID_INDEX)
            return 7;
    return 0;
}

int main(void) {
    pthread_t threads[WORKER_COUNT];
    pthread_t transfer_threads[WORKER_COUNT];
    uint32_t created = 0u;

    for (uint32_t cpu = 0u; cpu < TEST_CPU_COUNT; ++cpu)
        runqueue_init(&queues[cpu], cpu);
    for (uint32_t i = 0u; i < NODE_COUNT; ++i)
        runqueue_node_init(&nodes[i]);

    for (uint32_t i = 0u; i < WORKER_COUNT; ++i) {
        arguments[i].cpu = i / WORKERS_PER_CPU;
        arguments[i].worker = i;
        if (pthread_create(&threads[i], 0, run_worker, &arguments[i]) != 0) {
            fail_test();
            break;
        }
        ++created;
    }
    for (uint32_t i = 0u; i < created; ++i)
        if (pthread_join(threads[i], 0) != 0)
            fail_test();
    if (created != WORKER_COUNT ||
        __atomic_load_n(&failed, __ATOMIC_ACQUIRE) != 0u)
        return 1;

    for (uint32_t worker = 0u; worker < WORKER_COUNT; ++worker) {
        uint32_t owner = worker / WORKERS_PER_CPU;
        uint32_t first = worker * NODES_PER_WORKER;
        spinlock_acquire(&queues[owner].lock);
        for (uint32_t i = 0u; i < NODES_PER_WORKER; ++i) {
            uint32_t index = first + i;
            if (runqueue_enqueue_locked(&queues[owner], nodes, NODE_COUNT,
                    index, (worker + i) % LIONOS_RUNQUEUE_PRIORITY_LEVELS) != 0) {
                fail_test();
                break;
            }
        }
        spinlock_release(&queues[owner].lock);
    }
    if (__atomic_load_n(&failed, __ATOMIC_ACQUIRE) != 0u)
        return 2;

    /* Run cross-CPU transfers and priority changes concurrently. Both
     * operations contend on the same queue locks; priority workers must
     * follow a node when its owner changes rather than reading queue_cpu
     * without synchronization or failing on a stale owner snapshot. */
    pthread_t priority_threads[WORKER_COUNT];
    struct priority_argument priority_arguments[WORKER_COUNT];
    volatile uint32_t concurrent_start = 0u;
    uint32_t transfer_created = 0u;
    uint32_t priority_created = 0u;

    for (uint32_t worker = 0u; worker < WORKER_COUNT; ++worker) {
        transfer_arguments[worker].worker = worker;
        transfer_arguments[worker].start = &concurrent_start;
        if (pthread_create(&transfer_threads[worker], 0,
                run_transfer_worker, &transfer_arguments[worker]) != 0) {
            fail_test();
            break;
        }
        ++transfer_created;
    }
    for (uint32_t worker = 0u; worker < WORKER_COUNT; ++worker) {
        priority_arguments[worker].worker = worker;
        priority_arguments[worker].start = &concurrent_start;
        if (pthread_create(&priority_threads[worker], 0,
                run_priority_worker, &priority_arguments[worker]) != 0) {
            fail_test();
            break;
        }
        ++priority_created;
    }

    __atomic_store_n(&transfer_first_snapshot_expected, transfer_created,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&concurrent_start, 1u, __ATOMIC_RELEASE);
    for (uint32_t i = 0u; i < transfer_created; ++i)
        if (pthread_join(transfer_threads[i], 0) != 0)
            fail_test();
    for (uint32_t i = 0u; i < priority_created; ++i)
        if (pthread_join(priority_threads[i], 0) != 0)
            fail_test();
    if (transfer_created != WORKER_COUNT ||
        priority_created != WORKER_COUNT ||
        __atomic_load_n(&failed, __ATOMIC_ACQUIRE) != 0u ||
        __atomic_load_n(&transfer_stale_owner_retries, __ATOMIC_ACQUIRE) <
            WORKER_COUNT - 1u)
        return 6;

    /* Lock every queue to verify global membership and total accounting. */
    for (uint32_t cpu = 0u; cpu < TEST_CPU_COUNT; ++cpu)
        spinlock_acquire(&queues[cpu].lock);

    uint32_t global_total = 0u;
    for (uint32_t cpu = 0u; cpu < TEST_CPU_COUNT; ++cpu) {
        uint32_t total = 0u;
        if (runqueue_validate_locked(&queues[cpu], nodes,
                                     NODE_COUNT, &total) != 0 ||
            total != queues[cpu].total ||
            total > NODE_COUNT - global_total) {
            fail_test();
            break;
        }
        global_total += total;
    }
    if (global_total != NODE_COUNT)
        fail_test();

    for (uint32_t i = 0u; i < NODE_COUNT &&
         __atomic_load_n(&failed, __ATOMIC_ACQUIRE) == 0u; ++i) {
        uint32_t memberships = 0u;
        for (uint32_t cpu = 0u; cpu < TEST_CPU_COUNT; ++cpu) {
            for (uint32_t priority = 0u;
                 priority < LIONOS_RUNQUEUE_PRIORITY_LEVELS; ++priority) {
                uint32_t index = queues[cpu].head[priority];
                uint32_t visited = 0u;
                while (index != LIONOS_RUNQUEUE_INVALID_INDEX) {
                    if (index >= NODE_COUNT || visited++ >= NODE_COUNT) {
                        fail_test();
                        break;
                    }
                    if (index == i)
                        ++memberships;
                    index = nodes[index].next;
                }
            }
        }
        if (memberships != 1u ||
            nodes[i].queued != 1u ||
            nodes[i].queue_cpu >= TEST_CPU_COUNT)
            fail_test();
    }

    if (__atomic_load_n(&failed, __ATOMIC_ACQUIRE) == 0u) {
        for (uint32_t i = 0u; i < NODE_COUNT; ++i) {
            uint32_t owner = nodes[i].queue_cpu;
            if (owner >= TEST_CPU_COUNT ||
                runqueue_dequeue_locked(&queues[owner], nodes,
                                        NODE_COUNT, i) != 0) {
                fail_test();
                break;
            }
        }
    }

    for (uint32_t cpu = 0u; cpu < TEST_CPU_COUNT; ++cpu) {
        uint32_t total = 1u;
        if (runqueue_validate_locked(&queues[cpu], nodes,
                                     NODE_COUNT, &total) != 0 ||
            total != 0u)
            fail_test();
    }
    for (uint32_t cpu = TEST_CPU_COUNT; cpu > 0u; --cpu)
        spinlock_release(&queues[cpu - 1u].lock);

    if (simultaneous_busy_destination_test() != 0)
        return 5;
    /* Repeat the complete lifecycle to catch stale state across test runs. */
    for (uint32_t repetition = 0u; repetition < 3u; ++repetition) {
        if (sustained_concurrent_arrival_test() != 0)
            return 8;
    }
    if (shared_node_transfer_race_test() != 0)
        return 7;
    return __atomic_load_n(&failed, __ATOMIC_ACQUIRE) == 0u ? 0 : 4;
}
