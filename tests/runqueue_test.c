#include <stdint.h>
#include "runqueue.h"
#include "scheduler_policy.h"

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
    uint32_t balance_cursor[CPU_COUNT] = {0u, 0u, 0u};

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
        uint32_t queue_totals[CPU_COUNT];
        for (uint32_t cpu = 0u; cpu < CPU_COUNT; ++cpu)
            queue_totals[cpu] = queues[cpu].total;
        uint32_t victim = scheduler_select_victim(
            queue_totals, CPU_COUNT, destination,
            balance_cursor[destination], 0u);

        uint32_t moved_this_invocation = 0u;
        if (victim != LIONOS_RUNQUEUE_INVALID_INDEX &&
            runqueue_should_balance(queues[destination].total,
                                    queue_totals[victim], 0)) {
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
            balance_cursor[destination] = (victim + 1u) % CPU_COUNT;
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
        uint32_t queue_totals[CPU_COUNT];
        for (uint32_t cpu = 0u; cpu < CPU_COUNT; ++cpu)
            queue_totals[cpu] = queues[cpu].total;
        uint32_t victim = scheduler_select_victim(
            queue_totals, CPU_COUNT, destination,
            balance_cursor[destination], 0u);
        if (victim != LIONOS_RUNQUEUE_INVALID_INDEX &&
            runqueue_should_balance(queues[destination].total,
                                    queue_totals[victim], 0))
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


/*
 * Sustained arrivals below aggregate service capacity must not create an
 * unbounded source backlog. The source starts with a finite burst, receives
 * one new node per round, and all three CPUs dispatch at most one local node
 * per round. Busy destinations may each accept at most one transfer after
 * their dispatch. This is a deterministic queue-policy model, not production
 * scheduler timing or process-eligibility coverage.
 */
static int busy_balance_sustained_service_test(void) {
    enum {
        CPU_COUNT = 3u,
        INITIAL_BACKLOG = 32u,
        ARRIVAL_ROUNDS = 48u,
        TOTAL_NODES = INITIAL_BACKLOG + ARRIVAL_ROUNDS
    };
    struct runqueue queues[CPU_COUNT];
    struct runqueue_node nodes[TOTAL_NODES];
    uint8_t completed_seen[TOTAL_NODES];
    uint32_t transfers[CPU_COUNT] = {0u, 0u, 0u};
    uint32_t completed = 0u;
    uint32_t balance_cursor[CPU_COUNT] = {0u, 0u, 0u};
    uint32_t maximum_outstanding = 0u;

    for (uint32_t cpu = 0u; cpu < CPU_COUNT; ++cpu) {
        runqueue_init(&queues[cpu], cpu);
        queues[cpu].lock.value = 1u;
    }
    for (uint32_t i = 0u; i < TOTAL_NODES; ++i) {
        runqueue_node_init(&nodes[i]);
        completed_seen[i] = 0u;
    }
    for (uint32_t i = 0u; i < INITIAL_BACKLOG; ++i) {
        if (runqueue_enqueue_locked(&queues[0], nodes, TOTAL_NODES,
                                    i, i % LIONOS_RUNQUEUE_PRIORITY_LEVELS) != 0)
            return 1;
    }

    for (uint32_t round = 0u; round < ARRIVAL_ROUNDS; ++round) {
        uint32_t arrival = INITIAL_BACKLOG + round;
        if (runqueue_enqueue_locked(&queues[0], nodes, TOTAL_NODES,
                arrival, round % LIONOS_RUNQUEUE_PRIORITY_LEVELS) != 0)
            return 2;

        uint32_t outstanding = 0u;
        for (uint32_t cpu = 0u; cpu < CPU_COUNT; ++cpu)
            outstanding += queues[cpu].total;
        if (outstanding > maximum_outstanding)
            maximum_outstanding = outstanding;

        /* One local dispatch opportunity per CPU, including the source. */
        for (uint32_t cpu = 0u; cpu < CPU_COUNT; ++cpu) {
            uint32_t index = LIONOS_RUNQUEUE_INVALID_INDEX;
            int picked = runqueue_pick_next_locked(&queues[cpu], nodes,
                                                   TOTAL_NODES, &index);
            if (picked < 0) return 3;
            if (picked == 1) continue;
            if (index >= TOTAL_NODES || completed_seen[index] != 0u ||
                nodes[index].queued != 0u ||
                nodes[index].queue_cpu != LIONOS_RUNQUEUE_INVALID_INDEX ||
                completed >= TOTAL_NODES)
                return 4;
            completed_seen[index] = 1u;
            ++completed;
        }

        /* Each busy destination gets one independent balancing opportunity. */
        for (uint32_t destination = 1u;
             destination < CPU_COUNT; ++destination) {
            uint32_t queue_totals[CPU_COUNT];
            for (uint32_t cpu = 0u; cpu < CPU_COUNT; ++cpu)
                queue_totals[cpu] = queues[cpu].total;
            uint32_t victim = scheduler_select_victim(
                queue_totals, CPU_COUNT, destination,
                balance_cursor[destination], 0u);
            if (victim == LIONOS_RUNQUEUE_INVALID_INDEX ||
                !runqueue_should_balance(queues[destination].total,
                                         queue_totals[victim], 0))
                continue;

            uint32_t candidate = LIONOS_RUNQUEUE_INVALID_INDEX;
            for (uint32_t priority = 0u;
                 priority < LIONOS_RUNQUEUE_PRIORITY_LEVELS; ++priority) {
                candidate = queues[victim].head[priority];
                if (candidate != LIONOS_RUNQUEUE_INVALID_INDEX) break;
            }
            if (candidate == LIONOS_RUNQUEUE_INVALID_INDEX ||
                runqueue_move_locked(&queues[victim], &queues[destination],
                                     nodes, TOTAL_NODES, candidate) != 0)
                return 5;
            ++transfers[destination];
            balance_cursor[destination] = (victim + 1u) % CPU_COUNT;
        }

        outstanding = 0u;
        for (uint32_t cpu = 0u; cpu < CPU_COUNT; ++cpu) {
            uint32_t validated_total = 0u;
            if (runqueue_validate_locked(&queues[cpu], nodes, TOTAL_NODES,
                                         &validated_total) != 0 ||
                validated_total != queues[cpu].total)
                return 6;
            outstanding += validated_total;
        }
        if (outstanding + completed != INITIAL_BACKLOG + round + 1u ||
            outstanding > INITIAL_BACKLOG)
            return 7;
    }

    /* The sustained arrival stream must leave only a bounded residual. */
    if (maximum_outstanding > INITIAL_BACKLOG + 1u ||
        queues[0].total > 1u || queues[1].total != 0u ||
        queues[2].total != 0u || transfers[1] == 0u ||
        transfers[2] == 0u)
        return 8;

    /* Drain any final residual and prove every accepted node completed once. */
    for (uint32_t cpu = 0u; cpu < CPU_COUNT; ++cpu) {
        for (;;) {
            uint32_t index = LIONOS_RUNQUEUE_INVALID_INDEX;
            int picked = runqueue_pick_next_locked(&queues[cpu], nodes,
                                                   TOTAL_NODES, &index);
            if (picked == 1) break;
            if (picked != 0 || index >= TOTAL_NODES ||
                completed_seen[index] != 0u || completed >= TOTAL_NODES)
                return 9;
            completed_seen[index] = 1u;
            ++completed;
        }
    }
    if (completed != TOTAL_NODES) return 10;
    for (uint32_t i = 0u; i < TOTAL_NODES; ++i)
        if (completed_seen[i] != 1u || nodes[i].queued != 0u ||
            nodes[i].queue_cpu != LIONOS_RUNQUEUE_INVALID_INDEX)
            return 11;
    return 0;
}

/*
 * Exercise FIFO order across priority lanes and a cross-CPU move. The moved
 * node must join the destination lane tail, while a priority change must
 * append to the new source lane tail without disturbing the remaining order.
 */
static int priority_fifo_transfer_order_test(void) {
    enum { TEST_NODES = 6u };
    struct runqueue source, destination;
    struct runqueue_node test_nodes[TEST_NODES];
    const uint32_t expected_source[] = {0u, 3u, 1u, 4u};
    const uint32_t expected_destination[] = {5u, 2u};
    uint32_t picked = LIONOS_RUNQUEUE_INVALID_INDEX;
    uint32_t total = 0u;

    runqueue_init(&source, 0u);
    runqueue_init(&destination, 1u);
    /* Model the caller-held locks; source CPU precedes destination CPU. */
    source.lock.value = 1u;
    destination.lock.value = 1u;
    for (uint32_t i = 0u; i < TEST_NODES; ++i)
        runqueue_node_init(&test_nodes[i]);

    if (runqueue_enqueue_locked(&source, test_nodes, TEST_NODES, 0u, 0u) != 0 ||
        runqueue_enqueue_locked(&source, test_nodes, TEST_NODES, 1u, 1u) != 0 ||
        runqueue_enqueue_locked(&source, test_nodes, TEST_NODES, 2u, 0u) != 0 ||
        runqueue_enqueue_locked(&source, test_nodes, TEST_NODES, 3u, 1u) != 0 ||
        runqueue_enqueue_locked(&source, test_nodes, TEST_NODES, 4u, 2u) != 0 ||
        runqueue_enqueue_locked(&destination, test_nodes, TEST_NODES, 5u, 0u) != 0)
        return 1;

    if (runqueue_move_locked(&source, &destination, test_nodes,
                             TEST_NODES, 2u) != 0)
        return 2;
    if (runqueue_requeue_priority_locked(&source, test_nodes,
                                         TEST_NODES, 3u, 0u) != 0)
        return 3;

    if (runqueue_validate_locked(&source, test_nodes, TEST_NODES, &total) != 0 ||
        total != 4u || source.total != 4u ||
        runqueue_validate_locked(&destination, test_nodes,
                                TEST_NODES, &total) != 0 ||
        total != 2u || destination.total != 2u)
        return 4;

    for (uint32_t i = 0u;
         i < (uint32_t)(sizeof(expected_source) / sizeof(expected_source[0]));
         ++i) {
        if (runqueue_pick_next_locked(&source, test_nodes, TEST_NODES,
                                      &picked) != 0 ||
            picked != expected_source[i])
            return 5;
    }
    if (runqueue_pick_next_locked(&source, test_nodes, TEST_NODES,
                                  &picked) != 1 ||
        picked != LIONOS_RUNQUEUE_INVALID_INDEX)
        return 6;

    for (uint32_t i = 0u;
         i < (uint32_t)(sizeof(expected_destination) /
                        sizeof(expected_destination[0]));
         ++i) {
        if (runqueue_pick_next_locked(&destination, test_nodes, TEST_NODES,
                                      &picked) != 0 ||
            picked != expected_destination[i])
            return 7;
    }
    if (runqueue_pick_next_locked(&destination, test_nodes, TEST_NODES,
                                  &picked) != 1 ||
        picked != LIONOS_RUNQUEUE_INVALID_INDEX)
        return 8;

    if (runqueue_validate_locked(&source, test_nodes, TEST_NODES, &total) != 0 ||
        total != 0u ||
        runqueue_validate_locked(&destination, test_nodes,
                                TEST_NODES, &total) != 0 ||
        total != 0u)
        return 9;
    for (uint32_t i = 0u; i < TEST_NODES; ++i) {
        if (test_nodes[i].queued != 0u ||
            test_nodes[i].queue_cpu != LIONOS_RUNQUEUE_INVALID_INDEX ||
            test_nodes[i].next != LIONOS_RUNQUEUE_INVALID_INDEX ||
            test_nodes[i].previous != LIONOS_RUNQUEUE_INVALID_INDEX)
            return 10;
    }
    return 0;
}

int main(void) {
    struct runqueue queue;
    struct runqueue_node nodes[NODE_COUNT];
    uint32_t picked = 0u, total = 0u;
    int result;

    if (priority_fifo_transfer_order_test() != 0) return 78;
    if (move_rejects_corrupt_source_test() != 0) return 75;
    if (busy_balance_sustained_arrival_test() != 0) return 76;
    if (busy_balance_sustained_service_test() != 0) return 77;

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
    /*
     * Even a no-op priority request must reject a structurally invalid queue;
     * it must not bypass the full invariant check just because the priority
     * already matches the node.
     */
    if ((result = check(runqueue_requeue_priority_locked(&queue, nodes,
                    NODE_COUNT, 0u, 1u) < 0, 83u))) return result;
    if (queue.total != 1u || queue.count[1] != 1u ||
        queue.head[1] != NODE_COUNT || queue.tail[1] != 0u ||
        nodes[0].queued != 1u || nodes[0].priority != 1u)
        return 84;
    /*
     * A priority change must validate every lane before unlinking the node.
     * Corrupt metadata in its current lane must leave the node and counts
     * untouched, even when the requested priority is a different lane.
     */
    if ((result = check(runqueue_requeue_priority_locked(&queue, nodes,
                    NODE_COUNT, 0u, 0u) < 0, 31u))) return result;
    if (queue.total != 1u || queue.count[1] != 1u ||
        queue.head[1] != NODE_COUNT || queue.tail[1] != 0u ||
        queue.count[0] != 0u || nodes[0].queued != 1u ||
        nodes[0].queue_cpu != queue.cpu || nodes[0].priority != 1u ||
        nodes[0].previous != LIONOS_RUNQUEUE_INVALID_INDEX ||
        nodes[0].next != LIONOS_RUNQUEUE_INVALID_INDEX)
        return 80;
    queue.head[1] = 0u;
    if (runqueue_validate_locked(&queue, nodes, NODE_COUNT, &total) != 0 ||
        total != 1u)
        return 81;
    if (runqueue_requeue_priority_locked(&queue, nodes, NODE_COUNT,
                                         0u, 0u) != 0 ||
        queue.total != 1u || queue.count[1] != 0u ||
        queue.count[0] != 1u || queue.head[0] != 0u ||
        queue.tail[0] != 0u || nodes[0].queued != 1u ||
        nodes[0].queue_cpu != queue.cpu || nodes[0].priority != 0u ||
        nodes[0].previous != LIONOS_RUNQUEUE_INVALID_INDEX ||
        nodes[0].next != LIONOS_RUNQUEUE_INVALID_INDEX)
        return 82;
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

    /*
     * Distinct queue objects must not claim the same logical CPU. Rejecting
     * this transfer before mutation preserves the single-owner invariant and
     * avoids treating two locks as separate owners for one run queue.
     */
    struct runqueue duplicate_cpu_source, duplicate_cpu_destination;
    struct runqueue_node duplicate_cpu_nodes[2];
    runqueue_init(&duplicate_cpu_source, 4u);
    runqueue_init(&duplicate_cpu_destination, 4u);
    duplicate_cpu_source.lock.value = 1u;
    duplicate_cpu_destination.lock.value = 1u;
    runqueue_node_init(&duplicate_cpu_nodes[0]);
    runqueue_node_init(&duplicate_cpu_nodes[1]);
    if (runqueue_enqueue_locked(&duplicate_cpu_source, duplicate_cpu_nodes,
            2u, 0u, 1u) != 0 ||
        runqueue_enqueue_locked(&duplicate_cpu_destination, duplicate_cpu_nodes,
            2u, 1u, 1u) != 0)
        return 85;
    if (runqueue_move_locked(&duplicate_cpu_source,
            &duplicate_cpu_destination, duplicate_cpu_nodes, 2u, 0u) == 0)
        return 86;
    if (duplicate_cpu_source.total != 1u ||
        duplicate_cpu_destination.total != 1u ||
        duplicate_cpu_nodes[0].queued != 1u ||
        duplicate_cpu_nodes[0].queue_cpu != duplicate_cpu_source.cpu ||
        duplicate_cpu_nodes[0].next != LIONOS_RUNQUEUE_INVALID_INDEX ||
        duplicate_cpu_nodes[0].previous != LIONOS_RUNQUEUE_INVALID_INDEX ||
        duplicate_cpu_nodes[1].queued != 1u ||
        duplicate_cpu_nodes[1].queue_cpu != duplicate_cpu_destination.cpu ||
        runqueue_validate_locked(&duplicate_cpu_source, duplicate_cpu_nodes,
            2u, &total) != 0 || total != 1u ||
        runqueue_validate_locked(&duplicate_cpu_destination, duplicate_cpu_nodes,
            2u, &total) != 0 || total != 1u)
        return 87;
    /*
     * Local mutations reject corruption in an unrelated lane and mismatched
     * aggregate accounting before changing any queue or node metadata.
     */
    struct runqueue lane_guard;
    struct runqueue_node lane_guard_nodes[4];
    runqueue_init(&lane_guard, 5u);
    lane_guard.lock.value = 1u;
    for (uint32_t i = 0u; i < 4u; ++i)
        runqueue_node_init(&lane_guard_nodes[i]);
    if (runqueue_enqueue_locked(&lane_guard, lane_guard_nodes, 4u,
                                0u, 0u) != 0 ||
        runqueue_enqueue_locked(&lane_guard, lane_guard_nodes, 4u,
                                1u, 1u) != 0)
        return 88;
    lane_guard.head[2] = 4u;
    if (runqueue_enqueue_locked(&lane_guard, lane_guard_nodes, 4u,
                                2u, 0u) == 0 ||
        runqueue_dequeue_locked(&lane_guard, lane_guard_nodes, 4u,
                                0u) == 0)
        return 89;
    if (lane_guard.total != 2u ||
        lane_guard.count[0] != 1u || lane_guard.head[0] != 0u ||
        lane_guard.tail[0] != 0u || lane_guard.count[1] != 1u ||
        lane_guard.head[1] != 1u || lane_guard.tail[1] != 1u ||
        lane_guard_nodes[0].next != LIONOS_RUNQUEUE_INVALID_INDEX ||
        lane_guard_nodes[0].previous != LIONOS_RUNQUEUE_INVALID_INDEX ||
        lane_guard_nodes[1].next != LIONOS_RUNQUEUE_INVALID_INDEX ||
        lane_guard_nodes[1].previous != LIONOS_RUNQUEUE_INVALID_INDEX ||
        lane_guard_nodes[2].queued != 0u ||
        lane_guard_nodes[2].queue_cpu != LIONOS_RUNQUEUE_INVALID_INDEX)
        return 90;
    lane_guard.head[2] = LIONOS_RUNQUEUE_INVALID_INDEX;
    /*
     * A singleton lane cannot advertise different head and tail nodes, even
     * when both endpoint records individually look owned and detached at
     * their respective boundaries.
     */
    lane_guard_nodes[2].queued = 1u;
    lane_guard_nodes[2].queue_cpu = lane_guard.cpu;
    lane_guard_nodes[2].priority = 1u;
    lane_guard_nodes[2].previous = 1u;
    lane_guard_nodes[2].next = LIONOS_RUNQUEUE_INVALID_INDEX;
    lane_guard.tail[1] = 2u;
    if (runqueue_enqueue_locked(&lane_guard, lane_guard_nodes, 4u,
                                2u, 0u) == 0 ||
        runqueue_dequeue_locked(&lane_guard, lane_guard_nodes, 4u,
                                1u) == 0 ||
        lane_guard.total != 2u || lane_guard.count[1] != 1u ||
        lane_guard.head[1] != 1u || lane_guard.tail[1] != 2u)
        return 93;
    lane_guard.tail[1] = 1u;
    runqueue_node_init(&lane_guard_nodes[2]);
    lane_guard.total = 3u;
    if (runqueue_enqueue_locked(&lane_guard, lane_guard_nodes, 4u,
                                2u, 0u) == 0 ||
        runqueue_dequeue_locked(&lane_guard, lane_guard_nodes, 4u,
                                0u) == 0 ||
        lane_guard.count[0] != 1u || lane_guard.count[1] != 1u ||
        lane_guard_nodes[2].queued != 0u ||
        lane_guard_nodes[2].queue_cpu != LIONOS_RUNQUEUE_INVALID_INDEX)
        return 91;
    lane_guard.total = 2u;
    if (runqueue_validate_locked(&lane_guard, lane_guard_nodes, 4u,
                                 &total) != 0 || total != 2u ||
        runqueue_enqueue_locked(&lane_guard, lane_guard_nodes, 4u,
                                2u, 0u) != 0 ||
        runqueue_dequeue_locked(&lane_guard, lane_guard_nodes, 4u,
                                0u) != 0 ||
        runqueue_dequeue_locked(&lane_guard, lane_guard_nodes, 4u,
                                1u) != 0 ||
        runqueue_dequeue_locked(&lane_guard, lane_guard_nodes, 4u,
                                2u) != 0 ||
        runqueue_validate_locked(&lane_guard, lane_guard_nodes, 4u,
                                 &total) != 0 || total != 0u)
        return 92;
    /*
     * Mutation guards must inspect interior links, not only lane endpoints.
     * Corrupt an interior forward link while preserving valid head/tail
     * metadata; enqueue and dequeue must both reject it without mutation.
     */
    struct runqueue interior_guard;
    struct runqueue_node interior_nodes[4];
    runqueue_init(&interior_guard, 6u);
    interior_guard.lock.value = 1u;
    for (uint32_t i = 0u; i < 4u; ++i)
        runqueue_node_init(&interior_nodes[i]);
    if (runqueue_enqueue_locked(&interior_guard, interior_nodes, 4u,
                                0u, 2u) != 0 ||
        runqueue_enqueue_locked(&interior_guard, interior_nodes, 4u,
                                1u, 2u) != 0 ||
        runqueue_enqueue_locked(&interior_guard, interior_nodes, 4u,
                                2u, 2u) != 0)
        return 94;
    interior_nodes[1].next = 1u;
    if (runqueue_enqueue_locked(&interior_guard, interior_nodes, 4u,
                                3u, 2u) == 0 ||
        runqueue_dequeue_locked(&interior_guard, interior_nodes, 4u,
                                0u) == 0)
        return 95;
    if (interior_guard.total != 3u ||
        interior_guard.count[2] != 3u ||
        interior_guard.head[2] != 0u ||
        interior_guard.tail[2] != 2u ||
        interior_nodes[0].next != 1u ||
        interior_nodes[1].next != 1u ||
        interior_nodes[1].previous != 0u ||
        interior_nodes[2].previous != 1u ||
        interior_nodes[3].queued != 0u ||
        interior_nodes[3].queue_cpu != LIONOS_RUNQUEUE_INVALID_INDEX)
        return 96;
    if (runqueue_validate_locked(&interior_guard, interior_nodes, 4u,
                                 &total) == 0)
        return 97;
    interior_nodes[1].next = 2u;
    if (runqueue_validate_locked(&interior_guard, interior_nodes, 4u,
                                 &total) != 0 || total != 3u ||
        runqueue_enqueue_locked(&interior_guard, interior_nodes, 4u,
                                3u, 2u) != 0)
        return 98;
    for (uint32_t expected = 0u; expected < 4u; ++expected) {
        uint32_t picked_index = LIONOS_RUNQUEUE_INVALID_INDEX;
        if (runqueue_pick_next_locked(&interior_guard, interior_nodes, 4u,
                                      &picked_index) != 0 ||
            picked_index != expected)
            return 99;
    }
    if (runqueue_pick_next_locked(&interior_guard, interior_nodes, 4u,
                                  &picked) != 1 ||
        runqueue_validate_locked(&interior_guard, interior_nodes, 4u,
                                 &total) != 0 || total != 0u)
        return 100;
    /*
     * Cross-queue transfer must validate every source link before mutation.
     * Keep source endpoints intact while corrupting an interior back-link;
     * a rejected transfer must leave both queues and every node unchanged.
     */
    struct runqueue move_guard_source, move_guard_destination;
    struct runqueue_node move_guard_nodes[4];
    runqueue_init(&move_guard_source, 7u);
    runqueue_init(&move_guard_destination, 8u);
    move_guard_source.lock.value = 1u;
    move_guard_destination.lock.value = 1u;
    for (uint32_t i = 0u; i < 4u; ++i)
        runqueue_node_init(&move_guard_nodes[i]);
    if (runqueue_enqueue_locked(&move_guard_source, move_guard_nodes, 4u,
                                0u, 1u) != 0 ||
        runqueue_enqueue_locked(&move_guard_source, move_guard_nodes, 4u,
                                1u, 1u) != 0 ||
        runqueue_enqueue_locked(&move_guard_source, move_guard_nodes, 4u,
                                2u, 1u) != 0 ||
        runqueue_enqueue_locked(&move_guard_destination, move_guard_nodes, 4u,
                                3u, 1u) != 0)
        return 101;
    move_guard_nodes[1].previous = 2u;
    if (runqueue_move_locked(&move_guard_source, &move_guard_destination,
                             move_guard_nodes, 4u, 0u) == 0)
        return 102;
    if (move_guard_source.total != 3u ||
        move_guard_source.count[1] != 3u ||
        move_guard_source.head[1] != 0u ||
        move_guard_source.tail[1] != 2u ||
        move_guard_destination.total != 1u ||
        move_guard_destination.count[1] != 1u ||
        move_guard_destination.head[1] != 3u ||
        move_guard_destination.tail[1] != 3u ||
        move_guard_nodes[0].next != 1u ||
        move_guard_nodes[1].previous != 2u ||
        move_guard_nodes[1].next != 2u ||
        move_guard_nodes[2].previous != 1u ||
        move_guard_nodes[3].queued != 1u ||
        move_guard_nodes[3].queue_cpu != move_guard_destination.cpu)
        return 103;
    move_guard_nodes[1].previous = 0u;
    if (runqueue_validate_locked(&move_guard_source, move_guard_nodes, 4u,
                                 &total) != 0 || total != 3u ||
        runqueue_move_locked(&move_guard_source, &move_guard_destination,
                             move_guard_nodes, 4u, 1u) != 0)
        return 104;
    if (runqueue_validate_locked(&move_guard_source, move_guard_nodes, 4u,
                                 &total) != 0 || total != 2u ||
        runqueue_validate_locked(&move_guard_destination, move_guard_nodes,
                                 4u, &total) != 0 || total != 2u ||
        move_guard_source.head[1] != 0u ||
        move_guard_source.tail[1] != 2u ||
        move_guard_nodes[0].next != 2u ||
        move_guard_nodes[2].previous != 0u ||
        move_guard_destination.head[1] != 3u ||
        move_guard_destination.tail[1] != 1u ||
        move_guard_nodes[3].next != 1u ||
        move_guard_nodes[1].previous != 3u ||
        move_guard_nodes[1].next != LIONOS_RUNQUEUE_INVALID_INDEX ||
        move_guard_nodes[1].queue_cpu != move_guard_destination.cpu)
        return 105;
    /*
     * A corrupt destination lane must reject a cross-queue transfer before
     * unlinking the otherwise-valid source node or changing destination data.
     */
    struct runqueue destination_guard_source, destination_guard_destination;
    struct runqueue_node destination_guard_nodes[5];
    runqueue_init(&destination_guard_source, 9u);
    runqueue_init(&destination_guard_destination, 10u);
    destination_guard_source.lock.value = 1u;
    destination_guard_destination.lock.value = 1u;
    for (uint32_t i = 0u; i < 5u; ++i)
        runqueue_node_init(&destination_guard_nodes[i]);
    if (runqueue_enqueue_locked(&destination_guard_source,
            destination_guard_nodes, 5u, 0u, 2u) != 0 ||
        runqueue_enqueue_locked(&destination_guard_destination,
            destination_guard_nodes, 5u, 1u, 2u) != 0 ||
        runqueue_enqueue_locked(&destination_guard_destination,
            destination_guard_nodes, 5u, 2u, 2u) != 0 ||
        runqueue_enqueue_locked(&destination_guard_destination,
            destination_guard_nodes, 5u, 3u, 2u) != 0)
        return 106;
    destination_guard_nodes[2].next = 2u;
    if (runqueue_move_locked(&destination_guard_source,
            &destination_guard_destination, destination_guard_nodes,
            5u, 0u) == 0)
        return 107;
    if (destination_guard_source.total != 1u ||
        destination_guard_source.count[2] != 1u ||
        destination_guard_source.head[2] != 0u ||
        destination_guard_source.tail[2] != 0u ||
        destination_guard_nodes[0].queued != 1u ||
        destination_guard_nodes[0].queue_cpu != destination_guard_source.cpu ||
        destination_guard_nodes[0].previous != LIONOS_RUNQUEUE_INVALID_INDEX ||
        destination_guard_nodes[0].next != LIONOS_RUNQUEUE_INVALID_INDEX ||
        destination_guard_destination.total != 3u ||
        destination_guard_destination.count[2] != 3u ||
        destination_guard_destination.head[2] != 1u ||
        destination_guard_destination.tail[2] != 3u ||
        destination_guard_nodes[1].next != 2u ||
        destination_guard_nodes[2].previous != 1u ||
        destination_guard_nodes[2].next != 2u ||
        destination_guard_nodes[3].previous != 2u)
        return 108;
    destination_guard_nodes[2].next = 3u;
    if (runqueue_validate_locked(&destination_guard_source,
            destination_guard_nodes, 5u, &total) != 0 || total != 1u ||
        runqueue_validate_locked(&destination_guard_destination,
            destination_guard_nodes, 5u, &total) != 0 || total != 3u ||
        runqueue_move_locked(&destination_guard_source,
            &destination_guard_destination, destination_guard_nodes,
            5u, 0u) != 0)
        return 109;
    if (runqueue_validate_locked(&destination_guard_source,
            destination_guard_nodes, 5u, &total) != 0 || total != 0u ||
        runqueue_validate_locked(&destination_guard_destination,
            destination_guard_nodes, 5u, &total) != 0 || total != 4u ||
        destination_guard_destination.head[2] != 1u ||
        destination_guard_destination.tail[2] != 0u ||
        destination_guard_nodes[3].next != 0u ||
        destination_guard_nodes[0].previous != 3u ||
        destination_guard_nodes[0].next != LIONOS_RUNQUEUE_INVALID_INDEX ||
        destination_guard_nodes[0].queue_cpu !=
            destination_guard_destination.cpu)
        return 110;
    return 0;
}
