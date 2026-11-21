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

struct worker_argument {
    uint32_t cpu;
    uint32_t worker;
};

struct transfer_argument {
    uint32_t worker;
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
            uint32_t owner = nodes[index].queue_cpu;
            uint32_t destination_cpu =
                transfer_random(&random_state) % TEST_CPU_COUNT;

            if (owner >= TEST_CPU_COUNT) {
                fail_test();
                return 0;
            }
            if (destination_cpu == owner)
                destination_cpu = (destination_cpu + 1u) % TEST_CPU_COUNT;

            lock_queue_pair(owner, destination_cpu);
            if (nodes[index].queue_cpu != owner ||
                runqueue_move_locked(&queues[owner],
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

#define BUSY_RACE_NODE_COUNT 8u

struct busy_race_argument {
    uint32_t destination_cpu;
    volatile uint32_t *start;
    int result;
};

static struct runqueue busy_race_queues[3];
static struct runqueue_node busy_race_nodes[BUSY_RACE_NODE_COUNT];

static void *run_busy_balance_racer(void *opaque) {
    struct busy_race_argument *argument =
        (struct busy_race_argument *)opaque;
    const uint32_t source_cpu = 0u;
    const uint32_t destination_cpu = argument->destination_cpu;
    struct runqueue *source = &busy_race_queues[source_cpu];
    struct runqueue *destination = &busy_race_queues[destination_cpu];

    while (__atomic_load_n(argument->start, __ATOMIC_ACQUIRE) == 0u)
        __asm__ volatile("pause");

    argument->result = 0;
    for (uint32_t attempt = 0u; attempt < 100000u; ++attempt) {
        uint32_t first = source_cpu < destination_cpu
            ? source_cpu : destination_cpu;
        uint32_t second = source_cpu < destination_cpu
            ? destination_cpu : source_cpu;
        if (!spinlock_try_acquire(&busy_race_queues[first].lock)) {
            __asm__ volatile("pause");
            continue;
        }
        if (!spinlock_try_acquire(&busy_race_queues[second].lock)) {
            spinlock_release(&busy_race_queues[first].lock);
            __asm__ volatile("pause");
            continue;
        }

        if (runqueue_validate_locked(source, busy_race_nodes,
                BUSY_RACE_NODE_COUNT, 0) != 0 ||
            runqueue_validate_locked(destination, busy_race_nodes,
                BUSY_RACE_NODE_COUNT, 0) != 0) {
            argument->result = -1;
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
                runqueue_move_locked(source, destination,
                    busy_race_nodes, BUSY_RACE_NODE_COUNT,
                    candidate) != 0) {
                argument->result = -1;
            } else {
                argument->result = 1;
            }
        }

        spinlock_release(&busy_race_queues[second].lock);
        spinlock_release(&busy_race_queues[first].lock);
        if (argument->result != 0)
            return 0;
        __asm__ volatile("pause");
    }
    return 0;
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
        if (runqueue_enqueue(&busy_race_queues[0], busy_race_nodes,
                BUSY_RACE_NODE_COUNT, i, 2u) != 0)
            return 1;
    if (runqueue_enqueue(&busy_race_queues[1], busy_race_nodes,
            BUSY_RACE_NODE_COUNT, 4u, 2u) != 0 ||
        runqueue_enqueue(&busy_race_queues[2], busy_race_nodes,
            BUSY_RACE_NODE_COUNT, 5u, 2u) != 0)
        return 2;

    /* Each destination has a running task selected outside its READY queue. */
    for (uint32_t cpu = 1u; cpu <= 2u; ++cpu) {
        if (runqueue_pick_next(&busy_race_queues[cpu], busy_race_nodes,
                BUSY_RACE_NODE_COUNT, &picked) != 0 || picked != cpu + 3u)
            return 3;
    }

    /* A held source lock must make a nonblocking stealer defer immediately. */
    spinlock_acquire(&busy_race_queues[0].lock);
    if (spinlock_try_acquire(&busy_race_queues[0].lock) != 0) {
        spinlock_release(&busy_race_queues[0].lock);
        return 4;
    }
    spinlock_release(&busy_race_queues[0].lock);

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
        runqueue_validate(&busy_race_queues[0], busy_race_nodes,
            BUSY_RACE_NODE_COUNT, &total) != 0 || total != 2u ||
        runqueue_validate(&busy_race_queues[1], busy_race_nodes,
            BUSY_RACE_NODE_COUNT, &total) != 0 || total != 1u ||
        runqueue_validate(&busy_race_queues[2], busy_race_nodes,
            BUSY_RACE_NODE_COUNT, &total) != 0 || total != 1u)
        return 7;

    for (uint32_t i = 0u; i < 4u; ++i) {
        uint32_t memberships = 0u;
        for (uint32_t cpu = 0u; cpu < 3u; ++cpu) {
            for (uint32_t priority = 0u;
                 priority < LIONOS_RUNQUEUE_PRIORITY_LEVELS; ++priority) {
                uint32_t index = busy_race_queues[cpu].head[priority];
                while (index != LIONOS_RUNQUEUE_INVALID_INDEX) {
                    if (index == i)
                        ++memberships;
                    index = busy_race_nodes[index].next;
                }
            }
        }
        if (memberships != 1u || busy_race_nodes[i].queued != 1u)
            return 8;
    }
    if (busy_race_nodes[4].queued != 0u ||
        busy_race_nodes[5].queued != 0u ||
        busy_race_queues[1].total + busy_race_queues[2].total != 2u)
        return 9;
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
    return __atomic_load_n(&failed, __ATOMIC_ACQUIRE) == 0u ? 0 : 4;
}
