#include <pthread.h>
#include <stdint.h>
#include "runqueue.h"

#define TEST_CPU_COUNT 2u
#define WORKERS_PER_CPU 2u
#define WORKER_COUNT (TEST_CPU_COUNT * WORKERS_PER_CPU)
#define NODES_PER_WORKER 8u
#define NODE_COUNT (WORKER_COUNT * NODES_PER_WORKER)
#define ITERATIONS 2000u

struct worker_argument {
    uint32_t cpu;
    uint32_t worker;
};

static struct runqueue queues[TEST_CPU_COUNT];
static struct runqueue_node nodes[NODE_COUNT];
static struct worker_argument arguments[WORKER_COUNT];
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
                                        index, priority) != 0) {
                fail_test();
                spinlock_release(&queue->lock);
                return 0;
            }
            if (runqueue_validate_locked(queue, nodes, NODE_COUNT, 0) != 0) {
                fail_test();
                spinlock_release(&queue->lock);
                return 0;
            }
        }

        for (uint32_t i = 0u; i < NODES_PER_WORKER; ++i) {
            if (runqueue_dequeue_locked(queue, nodes, NODE_COUNT,
                                        first + i) != 0) {
                fail_test();
                spinlock_release(&queue->lock);
                return 0;
            }
            if (runqueue_validate_locked(queue, nodes, NODE_COUNT, 0) != 0) {
                fail_test();
                spinlock_release(&queue->lock);
                return 0;
            }
        }
        spinlock_release(&queue->lock);
    }
    return 0;
}

#define TRANSFER_ITERATIONS 1000u
#define TRANSFER_NODES_PER_WORKER (NODE_COUNT / 2u)

struct transfer_argument { uint32_t worker; };

static void *run_transfer_worker(void *opaque) {
    const struct transfer_argument *argument =
        (const struct transfer_argument *)opaque;
    uint32_t first = argument->worker * TRANSFER_NODES_PER_WORKER;

    for (uint32_t round = 0u; round < TRANSFER_ITERATIONS; ++round) {
        /* Fixed CPU-index lock order prevents opposite-direction deadlocks. */
        spinlock_acquire(&queues[0].lock);
        spinlock_acquire(&queues[1].lock);
        for (uint32_t i = 0u; i < TRANSFER_NODES_PER_WORKER; ++i) {
            uint32_t index = first + i;
            uint32_t owner = nodes[index].queue_cpu;
            struct runqueue *source;
            struct runqueue *destination;
            if (owner > 1u) {
                fail_test();
                spinlock_release(&queues[1].lock);
                spinlock_release(&queues[0].lock);
                return 0;
            }
            source = &queues[owner];
            destination = &queues[owner ^ 1u];
            if (runqueue_move_locked(source, destination, nodes,
                                     NODE_COUNT, index) != 0 ||
                runqueue_validate_locked(&queues[0], nodes,
                                         NODE_COUNT, 0) != 0 ||
                runqueue_validate_locked(&queues[1], nodes,
                                         NODE_COUNT, 0) != 0) {
                fail_test();
                spinlock_release(&queues[1].lock);
                spinlock_release(&queues[0].lock);
                return 0;
            }
        }
        spinlock_release(&queues[1].lock);
        spinlock_release(&queues[0].lock);
    }
    return 0;
}

int main(void) {
    pthread_t threads[WORKER_COUNT];
    pthread_t transfer_threads[2];
    struct transfer_argument transfer_arguments[2];

    for (uint32_t cpu = 0u; cpu < TEST_CPU_COUNT; ++cpu)
        runqueue_init(&queues[cpu], cpu);
    for (uint32_t i = 0u; i < NODE_COUNT; ++i)
        runqueue_node_init(&nodes[i]);

    for (uint32_t i = 0u; i < WORKER_COUNT; ++i) {
        arguments[i].cpu = i / WORKERS_PER_CPU;
        arguments[i].worker = i;
        if (pthread_create(&threads[i], 0, run_worker, &arguments[i]) != 0)
            return 1;
    }
    for (uint32_t i = 0u; i < WORKER_COUNT; ++i) {
        if (pthread_join(threads[i], 0) != 0)
            return 2;
    }
    if (__atomic_load_n(&failed, __ATOMIC_ACQUIRE) != 0u)
        return 3;

    spinlock_acquire(&queues[0].lock);
    for (uint32_t i = 0u; i < NODE_COUNT; ++i) {
        if (runqueue_enqueue_locked(&queues[0], nodes, NODE_COUNT,
                                    i, i % LIONOS_RUNQUEUE_PRIORITY_LEVELS) != 0) {
            spinlock_release(&queues[0].lock);
            return 5;
        }
    }
    spinlock_release(&queues[0].lock);

    for (uint32_t i = 0u; i < 2u; ++i) {
        transfer_arguments[i].worker = i;
        if (pthread_create(&transfer_threads[i], 0, run_transfer_worker,
                           &transfer_arguments[i]) != 0)
            return 6;
    }
    for (uint32_t i = 0u; i < 2u; ++i)
        if (pthread_join(transfer_threads[i], 0) != 0)
            return 7;
    if (__atomic_load_n(&failed, __ATOMIC_ACQUIRE) != 0u)
        return 8;

    for (uint32_t cpu = 0u; cpu < TEST_CPU_COUNT; ++cpu) {
        uint32_t total = 0u;
        spinlock_acquire(&queues[cpu].lock);
        int valid = runqueue_validate_locked(&queues[cpu], nodes,
                                             NODE_COUNT, &total);
        spinlock_release(&queues[cpu].lock);
        if (valid != 0 || total > NODE_COUNT)
            return 9;
    }
    spinlock_acquire(&queues[0].lock);
    spinlock_acquire(&queues[1].lock);
    for (uint32_t i = 0u; i < NODE_COUNT; ++i) {
        uint32_t owner = nodes[i].queue_cpu;
        if (owner >= TEST_CPU_COUNT ||
            runqueue_dequeue_locked(&queues[owner], nodes,
                                    NODE_COUNT, i) != 0) {
            spinlock_release(&queues[1].lock);
            spinlock_release(&queues[0].lock);
            return 10;
        }
    }
    spinlock_release(&queues[1].lock);
    spinlock_release(&queues[0].lock);
    for (uint32_t cpu = 0u; cpu < TEST_CPU_COUNT; ++cpu) {
        uint32_t total = 1u;
        spinlock_acquire(&queues[cpu].lock);
        int valid = runqueue_validate_locked(&queues[cpu], nodes,
                                             NODE_COUNT, &total);
        spinlock_release(&queues[cpu].lock);
        if (valid != 0 || total != 0u)
            return 11;
    }
    return 0;
}
