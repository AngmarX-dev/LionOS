#include <stdint.h>
#include "runqueue.h"

#define NODE_COUNT 8u

static int check(int condition, uint32_t code) {
    return condition ? 0 : (int)code;
}

int main(void) {
    struct runqueue queue;
    struct runqueue_node nodes[NODE_COUNT];
    uint32_t picked = 0u, total = 0u;
    int result;

    runqueue_init(&queue, 1u);
    /* The locked API is exercised with its queue lock held. */
    queue.lock.value = 1u;
    for (uint32_t i = 0u; i < NODE_COUNT; ++i)
        runqueue_node_init(&nodes[i]);

    if ((result = check(runqueue_enqueue_locked(&queue, nodes, NODE_COUNT, 2u, 2u) == 0, 1u))) return result;
    if ((result = check(runqueue_enqueue_locked(&queue, nodes, NODE_COUNT, 3u, 2u) == 0, 2u))) return result;
    if ((result = check(runqueue_enqueue_locked(&queue, nodes, NODE_COUNT, 4u, 0u) == 0, 3u))) return result;
    if ((result = check(runqueue_enqueue_locked(&queue, nodes, NODE_COUNT, 2u, 1u) < 0, 4u))) return result;
    if ((result = check(runqueue_enqueue_locked(&queue, nodes, NODE_COUNT, 5u, LIONOS_RUNQUEUE_PRIORITY_LEVELS) < 0, 5u))) return result;
    if ((result = check(runqueue_validate_locked(&queue, nodes, NODE_COUNT, &total) == 0 && total == 3u, 6u))) return result;

    if ((result = check(runqueue_pick_next_locked(&queue, nodes, NODE_COUNT, &picked) == 0 && picked == 4u, 7u))) return result;
    if ((result = check(runqueue_requeue_priority_locked(&queue, nodes, NODE_COUNT, 3u, 0u) == 0, 8u))) return result;
    if ((result = check(runqueue_pick_next_locked(&queue, nodes, NODE_COUNT, &picked) == 0 && picked == 3u, 9u))) return result;
    if ((result = check(runqueue_pick_next_locked(&queue, nodes, NODE_COUNT, &picked) == 0 && picked == 2u, 10u))) return result;
    if ((result = check(runqueue_pick_next_locked(&queue, nodes, NODE_COUNT, &picked) == 1 && picked == LIONOS_RUNQUEUE_INVALID_INDEX, 11u))) return result;

    for (uint32_t i = 0u; i < NODE_COUNT; ++i) {
        uint32_t priority = (NODE_COUNT - i) % LIONOS_RUNQUEUE_PRIORITY_LEVELS;
        if (runqueue_enqueue_locked(&queue, nodes, NODE_COUNT, i, priority) != 0) return 12;
    }
    if ((result = check(runqueue_validate_locked(&queue, nodes, NODE_COUNT, &total) == 0 && total == NODE_COUNT, 13u))) return result;
    if ((result = check(runqueue_enqueue_locked(&queue, nodes, NODE_COUNT, 0u, 0u) < 0, 14u))) return result;
    if ((result = check(runqueue_dequeue_locked(&queue, nodes, NODE_COUNT, 7u) == 0, 15u))) return result;
    if ((result = check(runqueue_dequeue_locked(&queue, nodes, NODE_COUNT, 7u) < 0, 16u))) return result;

    for (uint32_t i = 0u; i < NODE_COUNT; ++i) {
        if (nodes[i].queued && runqueue_dequeue_locked(&queue, nodes, NODE_COUNT, i) != 0) return 17;
    }
    if ((result = check(runqueue_validate_locked(&queue, nodes, NODE_COUNT, &total) == 0 && total == 0u, 18u))) return result;

    /* A foreign CPU cannot unlink a node owned by this queue. */
    struct runqueue wrong_cpu;
    runqueue_init(&wrong_cpu, 2u);
    wrong_cpu.lock.value = 1u;
    if (runqueue_enqueue_locked(&queue, nodes, NODE_COUNT, 0u, 2u) != 0) return 23;
    if (runqueue_enqueue_locked(&wrong_cpu, nodes, NODE_COUNT, 1u, 2u) != 0) return 24;
    if ((result = check(runqueue_dequeue_locked(&wrong_cpu, nodes, NODE_COUNT, 0u) < 0, 25u))) return result;
    if (runqueue_dequeue_locked(&queue, nodes, NODE_COUNT, 0u) != 0) return 26;
    if (runqueue_dequeue_locked(&wrong_cpu, nodes, NODE_COUNT, 1u) != 0) return 27;

    /* A malformed empty/non-empty lane must not be reported as an empty queue. */
    if (runqueue_enqueue_locked(&queue, nodes, NODE_COUNT, 0u, 1u) != 0) return 28;
    queue.head[1] = NODE_COUNT;
    if ((result = check(runqueue_pick_next_locked(&queue, nodes, NODE_COUNT, &picked) < 0, 29u))) return result;
    if ((result = check(runqueue_requeue_priority_locked(&queue, nodes, NODE_COUNT, 0u, 1u) < 0, 30u))) return result;
    queue.head[1] = 0u;
    if (runqueue_dequeue_locked(&queue, nodes, NODE_COUNT, 0u) != 0) return 31;

    /* Repeated FIFO cycles exercise link reuse and count restoration. */
    for (uint32_t round = 0u; round < 128u; ++round) {
        for (uint32_t i = 0u; i < NODE_COUNT; ++i)
            if (runqueue_enqueue_locked(&queue, nodes, NODE_COUNT, i, i % LIONOS_RUNQUEUE_PRIORITY_LEVELS) != 0) return 19;
        if (runqueue_validate_locked(&queue, nodes, NODE_COUNT, &total) != 0 || total != NODE_COUNT) return 20;
        for (uint32_t priority = 0u; priority < LIONOS_RUNQUEUE_PRIORITY_LEVELS; ++priority) {
            for (uint32_t i = 0u; i < NODE_COUNT; ++i) {
                if (nodes[i].queued && nodes[i].priority == priority) {
                    if (runqueue_pick_next_locked(&queue, nodes, NODE_COUNT, &picked) != 0 || picked != i) return 21;
                }
            }
        }
        if (runqueue_validate_locked(&queue, nodes, NODE_COUNT, &total) != 0 || total != 0u) return 22;
    }
    return 0;
}
