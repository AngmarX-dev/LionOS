#include <stdint.h>
#include "runqueue.h"

#define NODE_COUNT 8u

static int check(int condition, uint32_t code) {
    return condition ? 0 : (int)code;
}


/*
 * Model repeated busy-destination scheduler opportunities across two CPUs.
 * Each invocation may transfer at most one READY node; a selected local task
 * is kept outside its queue. This validates the queue policy's bounded
 * convergence mechanics, not production process eligibility or SMP timing.
 */
static int busy_balance_convergence_test(void) {
    enum { CPU_COUNT = 3u, BALANCE_NODE_COUNT = 8u };
    struct runqueue queues[CPU_COUNT];
    struct runqueue_node nodes[BALANCE_NODE_COUNT];
    uint32_t selected[2] = {
        LIONOS_RUNQUEUE_INVALID_INDEX,
        LIONOS_RUNQUEUE_INVALID_INDEX
    };

    for (uint32_t cpu = 0u; cpu < CPU_COUNT; ++cpu) {
        runqueue_init(&queues[cpu], cpu);
        queues[cpu].lock.value = 1u;
    }
    for (uint32_t i = 0u; i < BALANCE_NODE_COUNT; ++i)
        runqueue_node_init(&nodes[i]);

    for (uint32_t i = 0u; i < 6u; ++i)
        if (runqueue_enqueue_locked(&queues[0], nodes, BALANCE_NODE_COUNT,
                                    i, 2u) != 0)
            return 1;
    if (runqueue_enqueue_locked(&queues[1], nodes, BALANCE_NODE_COUNT, 6u, 2u) != 0 ||
        runqueue_enqueue_locked(&queues[2], nodes, BALANCE_NODE_COUNT, 7u, 2u) != 0)
        return 2;

    for (uint32_t cpu = 1u; cpu < CPU_COUNT; ++cpu)
        if (runqueue_pick_next_locked(&queues[cpu], nodes, BALANCE_NODE_COUNT,
                                      &selected[cpu - 1u]) != 0 ||
            selected[cpu - 1u] != cpu + 5u)
            return 3;

    uint32_t transfers = 0u;
    for (uint32_t invocation = 0u; invocation < 16u; ++invocation) {
        uint32_t destination = 1u + (invocation & 1u);
        uint32_t victim = LIONOS_RUNQUEUE_INVALID_INDEX;
        uint32_t largest = 0u;
        for (uint32_t cpu = 0u; cpu < CPU_COUNT; ++cpu) {
            if (cpu != destination && queues[cpu].total > largest) {
                largest = queues[cpu].total;
                victim = cpu;
            }
        }

        uint32_t moved_this_invocation = 0u;
        if (victim != LIONOS_RUNQUEUE_INVALID_INDEX &&
            runqueue_should_balance(queues[destination].total,
                                    queues[victim].total, 0)) {
            uint32_t candidate = LIONOS_RUNQUEUE_INVALID_INDEX;
            for (uint32_t priority = 0u;
                 priority < LIONOS_RUNQUEUE_PRIORITY_LEVELS; ++priority) {
                if (queues[victim].head[priority] !=
                    LIONOS_RUNQUEUE_INVALID_INDEX) {
                    candidate = queues[victim].head[priority];
                    break;
                }
            }
            if (candidate == LIONOS_RUNQUEUE_INVALID_INDEX ||
                runqueue_move_locked(&queues[victim], &queues[destination],
                                     nodes, BALANCE_NODE_COUNT, candidate) != 0)
                return 4;
            moved_this_invocation = 1u;
            ++transfers;
        }
        if (moved_this_invocation > 1u)
            return 5;

        uint32_t total_sum = 0u;
        uint32_t minimum = BALANCE_NODE_COUNT;
        uint32_t maximum = 0u;
        for (uint32_t cpu = 0u; cpu < CPU_COUNT; ++cpu) {
            uint32_t total = 0u;
            if (runqueue_validate_locked(&queues[cpu], nodes,
                                         BALANCE_NODE_COUNT, &total) != 0 ||
                total != queues[cpu].total)
                return 6;
            total_sum += total;
            if (total < minimum) minimum = total;
            if (total > maximum) maximum = total;
        }
        if (total_sum != 6u || maximum - minimum <= 1u)
            break;
    }

    if (transfers != 4u || queues[0].total != 2u ||
        queues[1].total != 2u || queues[2].total != 2u)
        return 7;

    /*
     * Once the busy queues converge, repeated scheduler opportunities must
     * not move work back and forth. Exercise alternating destinations for a
     * sustained window and require stable queue totals and valid membership.
     */
    for (uint32_t invocation = 0u; invocation < 1024u; ++invocation) {
        uint32_t destination = 1u + (invocation & 1u);
        uint32_t victim_total = 0u;
        for (uint32_t cpu = 0u; cpu < CPU_COUNT; ++cpu) {
            if (cpu != destination && queues[cpu].total > victim_total)
                victim_total = queues[cpu].total;
        }
        if (runqueue_should_balance(queues[destination].total,
                                    victim_total, 0))
            return 10;
        for (uint32_t cpu = 0u; cpu < CPU_COUNT; ++cpu) {
            uint32_t total = 0u;
            if (runqueue_validate_locked(&queues[cpu], nodes,
                    BALANCE_NODE_COUNT, &total) != 0 ||
                total != 2u || total != queues[cpu].total)
                return 11;
        }
    }
    for (uint32_t i = 0u; i < 6u; ++i) {
        uint32_t memberships = 0u;
        for (uint32_t cpu = 0u; cpu < CPU_COUNT; ++cpu) {
            for (uint32_t priority = 0u;
                 priority < LIONOS_RUNQUEUE_PRIORITY_LEVELS; ++priority) {
                uint32_t index = queues[cpu].head[priority];
                uint32_t visited = 0u;
                while (index != LIONOS_RUNQUEUE_INVALID_INDEX) {
                    if (index >= BALANCE_NODE_COUNT ||
                        visited++ >= BALANCE_NODE_COUNT)
                        return 8;
                    if (index == i) ++memberships;
                    index = nodes[index].next;
                }
            }
        }
        if (memberships != 1u || nodes[i].queued != 1u ||
            nodes[i].queue_cpu >= CPU_COUNT)
            return 8;
    }
    for (uint32_t i = 0u; i < 2u; ++i)
        if (nodes[selected[i]].queued != 0u ||
            nodes[selected[i]].queue_cpu != LIONOS_RUNQUEUE_INVALID_INDEX)
            return 9;
    return 0;
}

/*
 * Apply a sustained arrival rate to one source while two busy destinations
 * each receive at most one transfer per scheduling opportunity. Each
 * destination immediately dispatches its transferred task, modeling service
 * without claiming to reproduce production scheduler timing or eligibility.
 * The source is intentionally overloaded (three arrivals, two completions),
 * so its backlog must grow by exactly one per round without losing ownership.
 */
static int busy_balance_sustained_arrival_test(void) {
    enum {
        CPU_COUNT = 3u,
        ROUNDS = 24u,
        ARRIVALS_PER_ROUND = 3u,
        SUSTAINED_NODE_COUNT = ROUNDS * ARRIVALS_PER_ROUND,
        EXPECTED_COMPLETIONS = ROUNDS * 2u
    };
    struct runqueue queues[CPU_COUNT];
    struct runqueue_node nodes[SUSTAINED_NODE_COUNT];
    uint32_t completed[EXPECTED_COMPLETIONS];
    uint8_t seen[SUSTAINED_NODE_COUNT];
    uint32_t next_node = 0u;
    uint32_t completion_count = 0u;
    uint32_t transfers[CPU_COUNT] = {0u, 0u, 0u};

    for (uint32_t cpu = 0u; cpu < CPU_COUNT; ++cpu) {
        runqueue_init(&queues[cpu], cpu);
        queues[cpu].lock.value = 1u;
    }
    for (uint32_t i = 0u; i < SUSTAINED_NODE_COUNT; ++i)
        runqueue_node_init(&nodes[i]);

    for (uint32_t round = 0u; round < ROUNDS; ++round) {
        for (uint32_t arrival = 0u; arrival < ARRIVALS_PER_ROUND; ++arrival) {
            uint32_t index = next_node++;
            uint32_t priority = (round + arrival) %
                               LIONOS_RUNQUEUE_PRIORITY_LEVELS;
            if (index >= SUSTAINED_NODE_COUNT ||
                runqueue_enqueue_locked(&queues[0], nodes, SUSTAINED_NODE_COUNT,
                                        index, priority) != 0)
                return 1;
        }

        for (uint32_t destination = 1u;
             destination < CPU_COUNT; ++destination) {
            uint32_t victim = 0u;
            for (uint32_t cpu = 0u; cpu < CPU_COUNT; ++cpu) {
                if (cpu != destination &&
                    queues[cpu].total > queues[victim].total)
                    victim = cpu;
            }
            if (victim == destination ||
                !runqueue_should_balance(queues[destination].total,
                                         queues[victim].total, 0))
                return 2;

            uint32_t candidate = LIONOS_RUNQUEUE_INVALID_INDEX;
            for (uint32_t priority = 0u;
                 priority < LIONOS_RUNQUEUE_PRIORITY_LEVELS; ++priority) {
                if (queues[victim].head[priority] !=
                    LIONOS_RUNQUEUE_INVALID_INDEX) {
                    candidate = queues[victim].head[priority];
                    break;
                }
            }
            if (candidate == LIONOS_RUNQUEUE_INVALID_INDEX ||
                runqueue_move_locked(&queues[victim], &queues[destination],
                                     nodes, SUSTAINED_NODE_COUNT, candidate) != 0)
                return 3;
            ++transfers[destination];

            uint32_t dispatched = LIONOS_RUNQUEUE_INVALID_INDEX;
            if (runqueue_pick_next_locked(&queues[destination], nodes,
                    SUSTAINED_NODE_COUNT, &dispatched) != 0 ||
                dispatched != candidate ||
                completion_count >= EXPECTED_COMPLETIONS)
                return 4;
            completed[completion_count++] = dispatched;
        }

        if (queues[0].total != round + 1u ||
            queues[1].total != 0u || queues[2].total != 0u)
            return 5;
        for (uint32_t cpu = 0u; cpu < CPU_COUNT; ++cpu) {
            uint32_t total = 0u;
            if (runqueue_validate_locked(&queues[cpu], nodes, SUSTAINED_NODE_COUNT,
                                         &total) != 0 ||
                total != queues[cpu].total)
                return 6;
        }
    }

    if (next_node != SUSTAINED_NODE_COUNT ||
        completion_count != EXPECTED_COMPLETIONS ||
        transfers[1] != ROUNDS || transfers[2] != ROUNDS ||
        queues[0].total != ROUNDS)
        return 7;

    for (uint32_t i = 0u; i < SUSTAINED_NODE_COUNT; ++i) seen[i] = 0u;
    for (uint32_t i = 0u; i < completion_count; ++i) {
        uint32_t index = completed[i];
        if (index >= SUSTAINED_NODE_COUNT || seen[index] != 0u ||
            nodes[index].queued != 0u ||
            nodes[index].queue_cpu != LIONOS_RUNQUEUE_INVALID_INDEX)
            return 8;
        seen[index] = 1u;
    }

    for (uint32_t priority = 0u;
         priority < LIONOS_RUNQUEUE_PRIORITY_LEVELS; ++priority) {
        uint32_t index = queues[0].head[priority];
        uint32_t visited = 0u;
        while (index != LIONOS_RUNQUEUE_INVALID_INDEX) {
            if (index >= SUSTAINED_NODE_COUNT || visited++ >= SUSTAINED_NODE_COUNT ||
                seen[index] != 0u || nodes[index].queue_cpu != queues[0].cpu)
                return 9;
            seen[index] = 1u;
            index = nodes[index].next;
        }
    }
    for (uint32_t i = 0u; i < SUSTAINED_NODE_COUNT; ++i)
        if (seen[i] != 1u) return 10;

    return 0;
}

/*
 * A failed cross-CPU move must leave both queues untouched when the source
 * chain is malformed. In particular, preflight validation must happen before
 * unlinking the requested node from the source lane.
 */
static int move_rejects_corrupt_source_test(void) {
    struct runqueue source, destination;
    struct runqueue_node nodes[3];
    uint32_t total = 0u;

    runqueue_init(&source, 0u);
    runqueue_init(&destination, 1u);
    source.lock.value = 1u;
    destination.lock.value = 1u;
    for (uint32_t i = 0u; i < 3u; ++i)
        runqueue_node_init(&nodes[i]);

    if (runqueue_enqueue_locked(&source, nodes, 3u, 0u, 1u) != 0 ||
        runqueue_enqueue_locked(&source, nodes, 3u, 1u, 1u) != 0 ||
        runqueue_enqueue_locked(&destination, nodes, 3u, 2u, 1u) != 0)
        return 1;

    /* Break the backward link while preserving the source head and tail. */
    nodes[1].previous = LIONOS_RUNQUEUE_INVALID_INDEX;
    if (runqueue_move_locked(&source, &destination, nodes, 3u, 0u) == 0)
        return 2;

    /* The failed operation must not partially unlink or append the node. */
    if (source.total != 2u || source.count[1] != 2u ||
        source.head[1] != 0u || source.tail[1] != 1u ||
        destination.total != 1u || destination.count[1] != 1u ||
        destination.head[1] != 2u || destination.tail[1] != 2u ||
        nodes[0].queued != 1u || nodes[0].queue_cpu != source.cpu ||
        nodes[0].next != 1u || nodes[1].queued != 1u ||
        nodes[1].queue_cpu != source.cpu)
        return 3;

    /* Restore the deliberate corruption and prove both queues still validate. */
    nodes[1].previous = 0u;
    if (runqueue_validate_locked(&source, nodes, 3u, &total) != 0 ||
        total != 2u ||
        runqueue_validate_locked(&destination, nodes, 3u, &total) != 0 ||
        total != 1u)
        return 4;

    if (runqueue_move_locked(&source, &destination, nodes, 3u, 0u) != 0)
        return 5;
    if (source.total != 1u || destination.total != 2u ||
        nodes[0].queue_cpu != destination.cpu || nodes[0].queued != 1u)
        return 6;
    return 0;
}

int main(void) {
    struct runqueue queue;
    struct runqueue_node nodes[NODE_COUNT];
    uint32_t picked = 0u, total = 0u;
    int result;

    if (move_rejects_corrupt_source_test() != 0) return 75;
    if (busy_balance_sustained_arrival_test() != 0) return 76;

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

    /* Multiple priority changes must append to the destination lane in FIFO order. */
    if ((result = check(runqueue_requeue_priority_locked(&queue, nodes, NODE_COUNT, 3u, 0u) == 0, 8u))) return result;
    if ((result = check(runqueue_requeue_priority_locked(&queue, nodes, NODE_COUNT, 2u, 0u) == 0, 9u))) return result;
    /* Reapplying the current priority must preserve the existing FIFO position. */
    if ((result = check(runqueue_requeue_priority_locked(&queue, nodes, NODE_COUNT, 2u, 0u) == 0, 10u))) return result;
    if ((result = check(runqueue_validate_locked(&queue, nodes, NODE_COUNT, &total) == 0 &&
                        total == 2u && queue.count[0] == 2u &&
                        queue.head[0] == 3u && queue.tail[0] == 2u, 11u))) return result;
    if ((result = check(runqueue_pick_next_locked(&queue, nodes, NODE_COUNT, &picked) == 0 && picked == 3u, 12u))) return result;
    if ((result = check(runqueue_pick_next_locked(&queue, nodes, NODE_COUNT, &picked) == 0 && picked == 2u, 13u))) return result;
    if ((result = check(runqueue_pick_next_locked(&queue, nodes, NODE_COUNT, &picked) == 1 && picked == LIONOS_RUNQUEUE_INVALID_INDEX, 14u))) return result;

    for (uint32_t i = 0u; i < NODE_COUNT; ++i) {
        uint32_t priority = (NODE_COUNT - i) % LIONOS_RUNQUEUE_PRIORITY_LEVELS;
        if (runqueue_enqueue_locked(&queue, nodes, NODE_COUNT, i, priority) != 0) return 15;
    }
    if ((result = check(runqueue_validate_locked(&queue, nodes, NODE_COUNT, &total) == 0 && total == NODE_COUNT, 16u))) return result;
    if ((result = check(runqueue_enqueue_locked(&queue, nodes, NODE_COUNT, 0u, 0u) < 0, 17u))) return result;
    if ((result = check(runqueue_dequeue_locked(&queue, nodes, NODE_COUNT, 7u) == 0, 18u))) return result;
    if ((result = check(runqueue_dequeue_locked(&queue, nodes, NODE_COUNT, 7u) < 0, 19u))) return result;

    for (uint32_t i = 0u; i < NODE_COUNT; ++i) {
        if (nodes[i].queued && runqueue_dequeue_locked(&queue, nodes, NODE_COUNT, i) != 0) return 20;
    }
    if ((result = check(runqueue_validate_locked(&queue, nodes, NODE_COUNT, &total) == 0 && total == 0u, 21u))) return result;

    /* A foreign CPU cannot unlink a node owned by this queue. */
    struct runqueue wrong_cpu;
    runqueue_init(&wrong_cpu, 2u);
    wrong_cpu.lock.value = 1u;
    if (runqueue_enqueue_locked(&queue, nodes, NODE_COUNT, 0u, 2u) != 0) return 22;
    if (runqueue_enqueue_locked(&wrong_cpu, nodes, NODE_COUNT, 1u, 2u) != 0) return 23;
    if ((result = check(runqueue_dequeue_locked(&wrong_cpu, nodes, NODE_COUNT, 0u) < 0, 24u))) return result;
    if ((result = check(runqueue_validate_locked(&queue, nodes, NODE_COUNT, &total) == 0 && total == 1u, 25u))) return result;
    if ((result = check(runqueue_validate_locked(&wrong_cpu, nodes, NODE_COUNT, &total) == 0 && total == 1u, 26u))) return result;
    if (runqueue_dequeue_locked(&queue, nodes, NODE_COUNT, 0u) != 0) return 27;
    if (runqueue_dequeue_locked(&wrong_cpu, nodes, NODE_COUNT, 1u) != 0) return 28;

    /*
     * A cross-CPU move preserves priority, updates ownership exactly once,
     * and appends at the destination lane's FIFO tail.
     */
    struct runqueue source, destination;
    runqueue_init(&source, 0u);
    runqueue_init(&destination, 1u);
    source.lock.value = 1u;
    destination.lock.value = 1u;
    for (uint32_t i = 0u; i < NODE_COUNT; ++i)
        runqueue_node_init(&nodes[i]);
    if (runqueue_enqueue_locked(&source, nodes, NODE_COUNT, 0u, 2u) != 0) return 37;
    if (runqueue_enqueue_locked(&source, nodes, NODE_COUNT, 1u, 2u) != 0) return 38;
    if (runqueue_enqueue_locked(&destination, nodes, NODE_COUNT, 2u, 2u) != 0) return 39;

    /*
     * A malformed destination must reject the transfer before unlinking the
     * source node. Restoring the damaged metadata must leave both queues valid.
     */
    destination.tail[2] = NODE_COUNT;
    if (runqueue_move_locked(&source, &destination, nodes, NODE_COUNT, 0u) == 0)
        return 52;
    if (source.total != 2u || source.head[2] != 0u ||
        source.tail[2] != 1u || source.count[2] != 2u ||
        nodes[0].queued != 1u || nodes[0].queue_cpu != source.cpu ||
        nodes[0].previous != LIONOS_RUNQUEUE_INVALID_INDEX ||
        nodes[0].next != 1u || nodes[1].previous != 0u ||
        nodes[1].next != LIONOS_RUNQUEUE_INVALID_INDEX ||
        destination.total != 1u || destination.head[2] != 2u ||
        destination.count[2] != 1u)
        return 53;
    destination.tail[2] = 2u;
    if (runqueue_validate_locked(&source, nodes, NODE_COUNT, &total) != 0 ||
        total != 2u ||
        runqueue_validate_locked(&destination, nodes, NODE_COUNT, &total) != 0 ||
        total != 1u)
        return 54;

    if (runqueue_move_locked(&source, &destination, nodes, NODE_COUNT, 0u) != 0) return 40;
    if (nodes[0].queue_cpu != 1u || nodes[0].priority != 2u ||
        nodes[0].queued != 1u || source.total != 1u ||
        destination.total != 2u) return 41;
    if (runqueue_validate_locked(&source, nodes, NODE_COUNT, &total) != 0 ||
        total != 1u) return 42;
    if (runqueue_validate_locked(&destination, nodes, NODE_COUNT, &total) != 0 ||
        total != 2u || destination.head[2] != 2u ||
        destination.tail[2] != 0u) return 43;
    if (runqueue_move_locked(&source, &destination, nodes, NODE_COUNT, 1u) != 0) return 44;
    if (runqueue_pick_next_locked(&destination, nodes, NODE_COUNT, &picked) != 0 ||
        picked != 2u) return 45;
    if (runqueue_pick_next_locked(&destination, nodes, NODE_COUNT, &picked) != 0 ||
        picked != 0u) return 46;
    if (runqueue_pick_next_locked(&destination, nodes, NODE_COUNT, &picked) != 0 ||
        picked != 1u) return 47;
    if (runqueue_move_locked(&source, &destination, nodes, NODE_COUNT, 1u) == 0) return 48;
    if (runqueue_move_locked(&source, &source, nodes, NODE_COUNT, 1u) == 0) return 49;
    if (runqueue_validate_locked(&source, nodes, NODE_COUNT, &total) != 0 ||
        total != 0u) return 50;
    if (runqueue_validate_locked(&destination, nodes, NODE_COUNT, &total) != 0 ||
        total != 0u) return 51;

    /* A malformed empty/non-empty lane must not be reported as an empty queue. */
    if (runqueue_enqueue_locked(&queue, nodes, NODE_COUNT, 0u, 1u) != 0) return 29;
    queue.head[1] = NODE_COUNT;
    if ((result = check(runqueue_pick_next_locked(&queue, nodes, NODE_COUNT, &picked) < 0, 30u))) return result;
    if ((result = check(runqueue_requeue_priority_locked(&queue, nodes, NODE_COUNT, 0u, 1u) < 0, 31u))) return result;
    queue.head[1] = 0u;
    if (runqueue_dequeue_locked(&queue, nodes, NODE_COUNT, 0u) != 0) return 32;

    /* Repeated FIFO cycles exercise link reuse and count restoration. */
    for (uint32_t round = 0u; round < 128u; ++round) {
        for (uint32_t i = 0u; i < NODE_COUNT; ++i)
            if (runqueue_enqueue_locked(&queue, nodes, NODE_COUNT, i, i % LIONOS_RUNQUEUE_PRIORITY_LEVELS) != 0) return 33;
        if (runqueue_validate_locked(&queue, nodes, NODE_COUNT, &total) != 0 || total != NODE_COUNT) return 34;
        for (uint32_t priority = 0u; priority < LIONOS_RUNQUEUE_PRIORITY_LEVELS; ++priority) {
            for (uint32_t i = 0u; i < NODE_COUNT; ++i) {
                if (nodes[i].queued && nodes[i].priority == priority) {
                    if (runqueue_pick_next_locked(&queue, nodes, NODE_COUNT, &picked) != 0 || picked != i) return 35;
                }
            }
        }
        if (runqueue_validate_locked(&queue, nodes, NODE_COUNT, &total) != 0 || total != 0u) return 36;
    }
    /* Bounded balancing policy: idle progress versus busy backlog skew. */
    if (!runqueue_should_balance(0u, 1u, 1)) return 52;
    if (runqueue_should_balance(1u, 1u, 1)) return 53;
    if (runqueue_should_balance(1u, 2u, 1)) return 54;
    if (runqueue_should_balance(0u, 0u, 1)) return 55;
    if (runqueue_should_balance(0u, 1u, 0)) return 56;
    if (!runqueue_should_balance(0u, 2u, 0)) return 57;
    if (runqueue_should_balance(3u, 4u, 0)) return 58;
    if (!runqueue_should_balance(3u, 5u, 0)) return 59;
    if (runqueue_should_balance(5u, 3u, 0)) return 60;
    if (runqueue_should_balance(UINT32_MAX - 1u, UINT32_MAX, 0)) return 61;
    if (!runqueue_should_balance(UINT32_MAX - 2u, UINT32_MAX, 0)) return 62;
    if (runqueue_should_balance(0u, 1u, 2)) return 63;

    if (busy_balance_convergence_test() != 0) return 74;

    /*
     * Busy-to-busy queue acceptance: the destination has already selected
     * one task (which must remain RUNNING and outside every queue). A source
     * backlog skew of one must not move work; after the skew reaches two,
     * exactly one READY node may move and the remaining skew must be below
     * the threshold. This tests queue mechanics, not process eligibility or
     * end-to-end SMP scheduling.
     */
    struct runqueue busy_source, busy_destination;
    runqueue_init(&busy_source, 2u);
    runqueue_init(&busy_destination, 3u);
    busy_source.lock.value = 1u;
    busy_destination.lock.value = 1u;
    for (uint32_t i = 0u; i < NODE_COUNT; ++i)
        runqueue_node_init(&nodes[i]);
    if (runqueue_enqueue_locked(&busy_source, nodes, NODE_COUNT, 0u, 2u) != 0 ||
        runqueue_enqueue_locked(&busy_source, nodes, NODE_COUNT, 1u, 2u) != 0 ||
        runqueue_enqueue_locked(&busy_destination, nodes, NODE_COUNT, 4u, 2u) != 0 ||
        runqueue_enqueue_locked(&busy_destination, nodes, NODE_COUNT, 5u, 2u) != 0)
        return 64;
    if (runqueue_pick_next_locked(&busy_destination, nodes, NODE_COUNT,
                                  &picked) != 0 || picked != 4u)
        return 65;
    if (runqueue_should_balance(busy_destination.total,
                                busy_source.total, 0))
        return 66;
    if (runqueue_enqueue_locked(&busy_source, nodes, NODE_COUNT, 2u, 2u) != 0)
        return 67;
    if (!runqueue_should_balance(busy_destination.total,
                                 busy_source.total, 0))
        return 68;
    if (runqueue_move_locked(&busy_source, &busy_destination, nodes,
                             NODE_COUNT, 0u) != 0)
        return 69;
    if (busy_source.total != 2u || busy_destination.total != 2u ||
        nodes[0].queued != 1u || nodes[0].queue_cpu != busy_destination.cpu ||
        nodes[4].queued != 0u || picked != 4u)
        return 70;
    if (runqueue_should_balance(busy_destination.total,
                                busy_source.total, 0))
        return 71;
    if (runqueue_validate_locked(&busy_source, nodes, NODE_COUNT, &total) != 0 ||
        total != 2u ||
        runqueue_validate_locked(&busy_destination, nodes, NODE_COUNT, &total) != 0 ||
        total != 2u)
        return 72;
    uint32_t busy_memberships = 0u;
    for (uint32_t cpu = 0u; cpu < 2u; ++cpu) {
        struct runqueue *owner = cpu == 0u ? &busy_source : &busy_destination;
        for (uint32_t priority = 0u;
             priority < LIONOS_RUNQUEUE_PRIORITY_LEVELS; ++priority) {
            uint32_t index = owner->head[priority];
            while (index != LIONOS_RUNQUEUE_INVALID_INDEX) {
                if (index == 0u) ++busy_memberships;
                index = nodes[index].next;
            }
        }
    }
    if (busy_memberships != 1u)
        return 73;
    return 0;
}
