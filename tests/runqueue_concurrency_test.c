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

    return __atomic_load_n(&failed, __ATOMIC_ACQUIRE) == 0u ? 0 : 4;
}
