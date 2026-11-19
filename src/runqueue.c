#include "runqueue.h"

void runqueue_node_init(struct runqueue_node *node) {
    if (!node) return;
    node->next = LIONOS_RUNQUEUE_INVALID_INDEX;
    node->previous = LIONOS_RUNQUEUE_INVALID_INDEX;
    node->queue_cpu = LIONOS_RUNQUEUE_INVALID_INDEX;
    node->priority = 0u;
    node->queued = 0u;
}

void runqueue_init(struct runqueue *queue, uint32_t cpu) {
    if (!queue) return;
    queue->lock.value = 0u;
    queue->cpu = cpu;
    queue->total = 0u;
    for (uint32_t i = 0u; i < LIONOS_RUNQUEUE_PRIORITY_LEVELS; ++i) {
        queue->head[i] = LIONOS_RUNQUEUE_INVALID_INDEX;
        queue->tail[i] = LIONOS_RUNQUEUE_INVALID_INDEX;
        queue->count[i] = 0u;
    }
}

static int lane_valid(const struct runqueue *queue,
                      const struct runqueue_node *nodes,
                      uint32_t capacity, uint32_t priority) {
    uint32_t head, tail, count;
    if (!queue || !nodes || !capacity ||
        priority >= LIONOS_RUNQUEUE_PRIORITY_LEVELS)
        return 0;
    head = queue->head[priority];
    tail = queue->tail[priority];
    count = queue->count[priority];
    if (count == 0u)
        return head == LIONOS_RUNQUEUE_INVALID_INDEX &&
               tail == LIONOS_RUNQUEUE_INVALID_INDEX;
    if (count > capacity || head >= capacity || tail >= capacity)
        return 0;
    return nodes[head].queued == 1u &&
           nodes[head].queue_cpu == queue->cpu &&
           nodes[head].priority == priority &&
           nodes[head].previous == LIONOS_RUNQUEUE_INVALID_INDEX &&
           nodes[tail].queued == 1u &&
           nodes[tail].queue_cpu == queue->cpu &&
           nodes[tail].priority == priority &&
           nodes[tail].next == LIONOS_RUNQUEUE_INVALID_INDEX;
}

int runqueue_enqueue_locked(struct runqueue *queue,
                            struct runqueue_node *nodes,
                            uint32_t capacity, uint32_t index,
                            uint32_t priority) {
    struct runqueue_node *node;
    uint32_t tail;
    if (!queue || !nodes || !capacity || index >= capacity ||
        priority >= LIONOS_RUNQUEUE_PRIORITY_LEVELS ||
        queue->total >= capacity ||
        !lane_valid(queue, nodes, capacity, priority))
        return -1;
    node = &nodes[index];
    if (node->queued != 0u ||
        node->next != LIONOS_RUNQUEUE_INVALID_INDEX ||
        node->previous != LIONOS_RUNQUEUE_INVALID_INDEX ||
        node->queue_cpu != LIONOS_RUNQUEUE_INVALID_INDEX)
        return -1;
    tail = queue->tail[priority];
    if (tail != LIONOS_RUNQUEUE_INVALID_INDEX) {
        if (tail >= capacity || nodes[tail].next !=
            LIONOS_RUNQUEUE_INVALID_INDEX)
            return -1;
        nodes[tail].next = index;
        node->previous = tail;
    } else {
        if (queue->head[priority] != LIONOS_RUNQUEUE_INVALID_INDEX ||
            queue->count[priority] != 0u)
            return -1;
        queue->head[priority] = index;
    }
    node->next = LIONOS_RUNQUEUE_INVALID_INDEX;
    node->queue_cpu = queue->cpu;
    node->priority = priority;
    node->queued = 1u;
    queue->tail[priority] = index;
    ++queue->count[priority];
    ++queue->total;
    return 0;
}

int runqueue_dequeue_locked(struct runqueue *queue,
                            struct runqueue_node *nodes,
                            uint32_t capacity, uint32_t index) {
    struct runqueue_node *node;
    uint32_t priority, previous, next;
    if (!queue || !nodes || !capacity || index >= capacity ||
        queue->total == 0u)
        return -1;
    node = &nodes[index];
    priority = node->priority;
    if (node->queued != 1u || node->queue_cpu != queue->cpu ||
        priority >= LIONOS_RUNQUEUE_PRIORITY_LEVELS ||
        queue->count[priority] == 0u ||
        !lane_valid(queue, nodes, capacity, priority))
        return -1;
    previous = node->previous;
    next = node->next;
    if (previous == index || next == index) return -1;
    if (previous == LIONOS_RUNQUEUE_INVALID_INDEX) {
        if (queue->head[priority] != index) return -1;
    } else if (previous >= capacity ||
               nodes[previous].queued != 1u ||
               nodes[previous].queue_cpu != queue->cpu ||
               nodes[previous].priority != priority ||
               nodes[previous].next != index) {
        return -1;
    }
    if (next == LIONOS_RUNQUEUE_INVALID_INDEX) {
        if (queue->tail[priority] != index) return -1;
    } else if (next >= capacity ||
               nodes[next].queued != 1u ||
               nodes[next].queue_cpu != queue->cpu ||
               nodes[next].priority != priority ||
               nodes[next].previous != index) {
        return -1;
    }
    if (queue->count[priority] == 1u &&
        (previous != LIONOS_RUNQUEUE_INVALID_INDEX ||
         next != LIONOS_RUNQUEUE_INVALID_INDEX))
        return -1;

    if (previous == LIONOS_RUNQUEUE_INVALID_INDEX)
        queue->head[priority] = next;
    else
        nodes[previous].next = next;
    if (next == LIONOS_RUNQUEUE_INVALID_INDEX)
        queue->tail[priority] = previous;
    else
        nodes[next].previous = previous;

    --queue->count[priority];
    --queue->total;
    runqueue_node_init(node);
    return 0;
}

int runqueue_pick_next_locked(struct runqueue *queue,
                              struct runqueue_node *nodes,
                              uint32_t capacity, uint32_t *index) {
    if (!queue || !nodes || !capacity || !index) return -1;
    *index = LIONOS_RUNQUEUE_INVALID_INDEX;
    for (uint32_t priority = 0u;
         priority < LIONOS_RUNQUEUE_PRIORITY_LEVELS; ++priority) {
        if (queue->count[priority] == 0u) continue;
        uint32_t candidate = queue->head[priority];
        if (candidate >= capacity ||
            runqueue_dequeue_locked(queue, nodes, capacity, candidate) != 0)
            return -1;
        *index = candidate;
        return 0;
    }
    return 1;
}

int runqueue_requeue_priority_locked(struct runqueue *queue,
                                     struct runqueue_node *nodes,
                                     uint32_t capacity, uint32_t index,
                                     uint32_t priority) {
    uint32_t old_priority;
    if (!queue || !nodes || !capacity || index >= capacity ||
        priority >= LIONOS_RUNQUEUE_PRIORITY_LEVELS)
        return -1;
    if (nodes[index].queued != 1u ||
        nodes[index].queue_cpu != queue->cpu)
        return -1;
    old_priority = nodes[index].priority;
    if (old_priority == priority) return 0;
    if (runqueue_validate_locked(queue, nodes, capacity, 0) != 0)
        return -1;
    if (runqueue_dequeue_locked(queue, nodes, capacity, index) != 0)
        return -1;
    if (runqueue_enqueue_locked(queue, nodes, capacity, index, priority) == 0)
        return 0;
    (void)runqueue_enqueue_locked(queue, nodes, capacity, index, old_priority);
    return -1;
}

int runqueue_validate_locked(const struct runqueue *queue,
                             const struct runqueue_node *nodes,
                             uint32_t capacity, uint32_t *total_out) {
    uint32_t total = 0u;
    if (total_out) *total_out = 0u;
    if (!queue || !nodes || !capacity || queue->total > capacity)
        return -1;
    for (uint32_t priority = 0u;
         priority < LIONOS_RUNQUEUE_PRIORITY_LEVELS; ++priority) {
        uint32_t index = queue->head[priority];
        uint32_t previous = LIONOS_RUNQUEUE_INVALID_INDEX;
        uint32_t visited = 0u;
        if (!lane_valid(queue, nodes, capacity, priority)) return -1;
        while (index != LIONOS_RUNQUEUE_INVALID_INDEX) {
            const struct runqueue_node *node;
            if (index >= capacity || visited >= capacity) return -1;
            node = &nodes[index];
            if (node->queued != 1u || node->queue_cpu != queue->cpu ||
                node->priority != priority || node->previous != previous)
                return -1;
            previous = index;
            index = node->next;
            ++visited;
        }
        if (visited != queue->count[priority] ||
            (visited && previous != queue->tail[priority]))
            return -1;
        total += visited;
    }
    if (total != queue->total) return -1;
    for (uint32_t i = 0u; i < capacity; ++i) {
        const struct runqueue_node *node = &nodes[i];
        if (node->queued == 0u) {
            if (node->next != LIONOS_RUNQUEUE_INVALID_INDEX ||
                node->previous != LIONOS_RUNQUEUE_INVALID_INDEX ||
                node->queue_cpu != LIONOS_RUNQUEUE_INVALID_INDEX)
                return -1;
        } else if (node->queued != 1u ||
                   node->queue_cpu != queue->cpu ||
                   node->priority >= LIONOS_RUNQUEUE_PRIORITY_LEVELS) {
            return -1;
        } else {
            uint32_t cursor = queue->head[node->priority];
            uint32_t occurrences = 0u;
            for (uint32_t step = 0u;
                 cursor != LIONOS_RUNQUEUE_INVALID_INDEX && step < capacity;
                 ++step) {
                if (cursor >= capacity) return -1;
                if (cursor == i) ++occurrences;
                cursor = nodes[cursor].next;
            }
            if (occurrences != 1u) return -1;
        }
    }
    if (total_out) *total_out = total;
    return 0;
}
