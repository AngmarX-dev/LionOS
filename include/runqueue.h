#ifndef LIONOS_RUNQUEUE_H
#define LIONOS_RUNQUEUE_H

#include <stdint.h>
#include "spinlock.h"

#define LIONOS_RUNQUEUE_PRIORITY_LEVELS 4u
#define LIONOS_RUNQUEUE_INVALID_INDEX UINT32_MAX

/*
 * Intrusive process-table-index links. A node is owned by at most one
 * run queue. Callers initialize nodes before first use.
 */
struct runqueue_node {
    uint32_t next;
    uint32_t previous;
    uint32_t queue_cpu;
    uint32_t priority;
    uint32_t queued;
};

/*
 * Queue operations suffixed _locked require the caller to hold lock.
 * Scheduler integration must acquire process_lock before this lock.
 */
struct runqueue {
    struct spinlock lock;
    uint32_t cpu;
    uint32_t head[LIONOS_RUNQUEUE_PRIORITY_LEVELS];
    uint32_t tail[LIONOS_RUNQUEUE_PRIORITY_LEVELS];
    uint32_t count[LIONOS_RUNQUEUE_PRIORITY_LEVELS];
    uint32_t total;
};

void runqueue_node_init(struct runqueue_node *node);
void runqueue_init(struct runqueue *queue, uint32_t cpu);
int runqueue_enqueue_locked(struct runqueue *queue,
                            struct runqueue_node *nodes,
                            uint32_t capacity, uint32_t index,
                            uint32_t priority);
int runqueue_dequeue_locked(struct runqueue *queue,
                            struct runqueue_node *nodes,
                            uint32_t capacity, uint32_t index);
int runqueue_pick_next_locked(struct runqueue *queue,
                              struct runqueue_node *nodes,
                              uint32_t capacity, uint32_t *index);
int runqueue_requeue_priority_locked(struct runqueue *queue,
                                     struct runqueue_node *nodes,
                                     uint32_t capacity, uint32_t index,
                                     uint32_t priority);
int runqueue_validate_locked(const struct runqueue *queue,
                             const struct runqueue_node *nodes,
                             uint32_t capacity, uint32_t *total_out);

#endif
