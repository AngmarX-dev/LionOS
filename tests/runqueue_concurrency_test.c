#include <pthread.h>
#include <stdint.h>
#include "runqueue.h"

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
    uint32_t first = argument->worker * NODES_PER_WORKER;
    uint32_t random_state =
        0x9E3779B9u ^ ((argument->worker + 1u) * 0x85EBCA6Bu);

    for (uint32_t round = 0u; round < TRANSFER_ITERATIONS; ++round) {
        for (uint32_t i = 0u; i < NODES_PER_WORKER; ++i) {
            uint32_t index = first + i;
            uint32_t owner = node_owner_snapshot(index);
            uint32_t destination_cpu =
                transfer_random(&random_state) % TEST_CPU_COUNT;

            if (owner >= TEST_CPU_COUNT) {
                fail_test();
                return 0;
            }
            if (destination_cpu == owner)
                destination_cpu = (destination_cpu + 1u) % TEST_CPU_COUNT;

            lock_queue_pair(owner, destination_cpu);
            /*
             * Another worker may have moved the node after the snapshot.
             * Revalidate under both locks and retry instead of treating this
             * ordinary concurrent ownership change as a test failure.
             */
            if (nodes[index].queue_cpu != owner) {
                unlock_queue_pair(owner, destination_cpu);
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
            uint32_t owner = nodes[index].queue_cpu;
            if (owner >= TEST_CPU_COUNT) {
                fail_test();
                return 0;
            }

            struct runqueue *queue = &queues[owner];
            spinlock_acquire(&queue->lock);
            if (nodes[index].queue_cpu != owner ||
                runqueue_requeue_priority_locked(queue, nodes, NODE_COUNT,
                    index, (round + i + argument->worker) %
                    LIONOS_RUNQUEUE_PRIORITY_LEVELS) != 0 ||
                runqueue_validate_locked(queue, nodes, NODE_COUNT, 0) != 0) {
                fail_test();
                spinlock_release(&queue->lock);
                return 0;
            }
            spinlock_release(&queue->lock);
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

struct shared_transfer_argument {
    uint32_t worker;
};

static void *run_shared_transfer_worker(void *opaque) {
    const struct shared_transfer_argument *argument =
        (const struct shared_transfer_argument *)opaque;

    for (uint32_t round = 0u; round < SHARED_TRANSFER_ITERATIONS; ++round) {
        const uint32_t index = 0u;
        uint32_t owner = node_owner_snapshot(index);
        uint32_t destination = (argument->worker + round) % TEST_CPU_COUNT;

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
    }
    return 0;
}

static int shared_node_transfer_race_test(void) {
    pthread_t threads[2];
    struct shared_transfer_argument arguments[2] = {{0u}, {1u}};
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

    created = 0u;
    for (uint32_t worker = 0u; worker < WORKER_COUNT; ++worker) {
        transfer_arguments[worker].worker = worker;
        if (pthread_create(&transfer_threads[worker], 0,
                run_transfer_worker, &transfer_arguments[worker]) != 0) {
            fail_test();
            break;
        }
        ++created;
    }
    for (uint32_t i = 0u; i < created; ++i)
        if (pthread_join(transfer_threads[i], 0) != 0)
            fail_test();
    if (created != WORKER_COUNT ||
        __atomic_load_n(&failed, __ATOMIC_ACQUIRE) != 0u)
        return 3;

    /*
     * Stress priority changes after transfers. Each worker owns disjoint
     * nodes, follows them to their current CPU, and requeues under that
     * queue's lock while other workers operate on the same queues.
     */
    pthread_t priority_threads[WORKER_COUNT];
    struct priority_argument priority_arguments[WORKER_COUNT];
    volatile uint32_t priority_start = 0u;
    created = 0u;
    for (uint32_t worker = 0u; worker < WORKER_COUNT; ++worker) {
        priority_arguments[worker].worker = worker;
        priority_arguments[worker].start = &priority_start;
        if (pthread_create(&priority_threads[worker], 0,
                run_priority_worker, &priority_arguments[worker]) != 0) {
            fail_test();
            break;
        }
        ++created;
    }
    __atomic_store_n(&priority_start, 1u, __ATOMIC_RELEASE);
    for (uint32_t i = 0u; i < created; ++i)
        if (pthread_join(priority_threads[i], 0) != 0)
            fail_test();
    if (created != WORKER_COUNT ||
        __atomic_load_n(&failed, __ATOMIC_ACQUIRE) != 0u)
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
    if (shared_node_transfer_race_test() != 0)
        return 7;
    return __atomic_load_n(&failed, __ATOMIC_ACQUIRE) == 0u ? 0 : 4;
}
