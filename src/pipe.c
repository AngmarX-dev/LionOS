#include <stdint.h>
#include "pipe.h"
#include "spinlock.h"
#include "process.h"

#define PIPE_INDEX_BITS 4u
#define PIPE_INDEX_MASK ((1u << PIPE_INDEX_BITS) - 1u)
#define PIPE_GENERATION_MAX 0x07FFFFFFu

struct pipe_state {
    uint32_t used;
    uint32_t generation;
    uint32_t readers;
    uint32_t writers;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint8_t data[LIONOS_PIPE_BUFFER];
};

static struct pipe_state pipes[LIONOS_PIPE_MAX];
static struct spinlock pipe_lock;

static uint32_t pipe_handle(uint32_t id, uint32_t side, uint32_t generation) {
    return (generation << 5) | (id << 1) | side;
}

static int decode(uint32_t handle, uint32_t *id, uint32_t *side,
                  uint32_t *generation) {
    uint32_t index = (handle >> 1) & PIPE_INDEX_MASK;
    uint32_t gen = handle >> 5;
    if (index >= LIONOS_PIPE_MAX || gen == 0u) return -1;
    if (id) *id = index;
    if (side) *side = handle & 1u;
    if (generation) *generation = gen;
    return 0;
}

static uint32_t pipe_channel(uint32_t id, uint32_t side) {
    return 0x50495000u | (id << 1) | side;
}

void pipe_init(void) {
    spinlock_init(&pipe_lock);
    for (uint32_t i = 0; i < LIONOS_PIPE_MAX; ++i) {
        pipes[i].used = 0u;
        pipes[i].generation = 0u;
        pipes[i].readers = 0u;
        pipes[i].writers = 0u;
        pipes[i].head = pipes[i].tail = pipes[i].count = 0u;
    }
}

int32_t pipe_create(uint32_t *read_end, uint32_t *write_end) {
    struct process *owner = process_pipe_owner();
    if (!owner || !read_end || !write_end) return -1;

    uint32_t irq = spinlock_irqsave_acquire(&pipe_lock);
    for (uint32_t i = 0; i < LIONOS_PIPE_MAX; ++i) {
        struct pipe_state *p = &pipes[i];
        uint32_t bit = 1u << i;
        if (p->used || (owner->pipe_read_mask & bit) ||
            (owner->pipe_write_mask & bit)) continue;

        uint32_t generation = p->generation + 1u;
        if (generation == 0u || generation > PIPE_GENERATION_MAX)
            generation = 1u;

        p->generation = generation;
        p->used = 1u;
        p->readers = 1u;
        p->writers = 1u;
        p->head = p->tail = p->count = 0u;
        owner->pipe_read_mask |= bit;
        owner->pipe_write_mask |= bit;
        *read_end = pipe_handle(i, 0u, generation);
        *write_end = pipe_handle(i, 1u, generation);
        spinlock_irqrestore_release(&pipe_lock, irq);
        return 0;
    }
    spinlock_irqrestore_release(&pipe_lock, irq);
    return -1;
}

/* The child is kept unschedulable by process_fork_current until this succeeds. */
int32_t pipe_fork(struct process *parent, struct process *child) {
    if (!parent || !child || parent == child) return -1;

    uint32_t irq = spinlock_irqsave_acquire(&pipe_lock);
    uint32_t reads = parent->pipe_read_mask;
    uint32_t writes = parent->pipe_write_mask;

    for (uint32_t i = 0; i < LIONOS_PIPE_MAX; ++i) {
        uint32_t bit = 1u << i;
        struct pipe_state *p = &pipes[i];
        if ((reads & bit) && (!p->used || p->readers == 0u)) {
            spinlock_irqrestore_release(&pipe_lock, irq);
            return -1;
        }
        if ((writes & bit) && (!p->used || p->writers == 0u)) {
            spinlock_irqrestore_release(&pipe_lock, irq);
            return -1;
        }
        if ((reads & bit) && p->readers == UINT32_MAX) {
            spinlock_irqrestore_release(&pipe_lock, irq);
            return -1;
        }
        if ((writes & bit) && p->writers == UINT32_MAX) {
            spinlock_irqrestore_release(&pipe_lock, irq);
            return -1;
        }
    }

    for (uint32_t i = 0; i < LIONOS_PIPE_MAX; ++i) {
        uint32_t bit = 1u << i;
        if (reads & bit) ++pipes[i].readers;
        if (writes & bit) ++pipes[i].writers;
    }
    child->pipe_read_mask = reads;
    child->pipe_write_mask = writes;
    spinlock_irqrestore_release(&pipe_lock, irq);
    return 0;
}

int32_t pipe_read(uint32_t handle, void *data, uint32_t capacity) {
    uint32_t id, side, generation;
    if (decode(handle, &id, &side, &generation) != 0 ||
        side != 0u || !data || !capacity) return -1;

    struct process *owner = process_pipe_owner();
    if (!owner) return -1;
    uint32_t bit = 1u << id;

    for (;;) {
        uint32_t irq = spinlock_irqsave_acquire(&pipe_lock);
        struct pipe_state *p = &pipes[id];
        if (!p->used || p->generation != generation ||
            !(owner->pipe_read_mask & bit)) {
            spinlock_irqrestore_release(&pipe_lock, irq);
            return -1;
        }
        if (p->count) {
            uint32_t n = p->count < capacity ? p->count : capacity;
            uint8_t *dst = data;
            for (uint32_t i = 0; i < n; ++i) {
                dst[i] = p->data[p->tail];
                p->tail = (p->tail + 1u) % LIONOS_PIPE_BUFFER;
            }
            p->count -= n;
            spinlock_irqrestore_release(&pipe_lock, irq);
            process_wakeup((uintptr_t)pipe_channel(id, 1u));
            return (int32_t)n;
        }
        if (!p->writers) {
            spinlock_irqrestore_release(&pipe_lock, irq);
            return 0;
        }
        int32_t blocked = process_sleep_on((uintptr_t)pipe_channel(id, 0u),
                                           &pipe_lock, irq);
        if (blocked == PROCESS_WAIT_BLOCKED) return blocked;
        if (blocked < 0) return blocked;
    }
}

int32_t pipe_write(uint32_t handle, const void *data, uint32_t length) {
    uint32_t id, side, generation;
    if (decode(handle, &id, &side, &generation) != 0 ||
        side != 1u || !data || !length) return -1;

    struct process *owner = process_pipe_owner();
    if (!owner) return -1;
    uint32_t bit = 1u << id;
    const uint8_t *src = data;
    uint32_t done = 0u;

    for (;;) {
        uint32_t irq = spinlock_irqsave_acquire(&pipe_lock);
        struct pipe_state *p = &pipes[id];
        if (!p->used || p->generation != generation ||
            !(owner->pipe_write_mask & bit) || !p->readers) {
            spinlock_irqrestore_release(&pipe_lock, irq);
            return done ? (int32_t)done : -1;
        }

        uint32_t space = LIONOS_PIPE_BUFFER - p->count;
        if (space) {
            uint32_t n = (length - done) < space ? (length - done) : space;
            for (uint32_t i = 0; i < n; ++i) {
                p->data[p->head] = src[done + i];
                p->head = (p->head + 1u) % LIONOS_PIPE_BUFFER;
            }
            p->count += n;
            done += n;
            spinlock_irqrestore_release(&pipe_lock, irq);
            process_wakeup((uintptr_t)pipe_channel(id, 0u));
            if (done == length) return (int32_t)done;
            continue;
        }

        int32_t blocked = process_sleep_on((uintptr_t)pipe_channel(id, 1u),
                                           &pipe_lock, irq);
        if (blocked == PROCESS_WAIT_BLOCKED)
            return done ? (int32_t)done : PIPE_BLOCKED;
        if (blocked < 0) return blocked;
    }
}

int32_t pipe_close(uint32_t handle) {
    uint32_t id, side, generation;
    if (decode(handle, &id, &side, &generation) != 0) return -1;

    struct process *owner = process_pipe_owner();
    if (!owner) return -1;
    uint32_t bit = 1u << id;
    uint32_t irq = spinlock_irqsave_acquire(&pipe_lock);
    struct pipe_state *p = &pipes[id];

    if (!p->used || p->generation != generation ||
        (side ? !(owner->pipe_write_mask & bit)
              : !(owner->pipe_read_mask & bit))) {
        spinlock_irqrestore_release(&pipe_lock, irq);
        return -1;
    }

    if (side) {
        owner->pipe_write_mask &= ~bit;
        if (p->writers) --p->writers;
    } else {
        owner->pipe_read_mask &= ~bit;
        if (p->readers) --p->readers;
    }
    if (!p->readers && !p->writers) p->used = 0u;
    spinlock_irqrestore_release(&pipe_lock, irq);

    process_wakeup((uintptr_t)pipe_channel(id, 0u));
    process_wakeup((uintptr_t)pipe_channel(id, 1u));
    return 0;
}

void pipe_close_all(struct process *owner) {
    if (!owner) return;
    uint32_t changed = 0u;
    uint32_t irq = spinlock_irqsave_acquire(&pipe_lock);
    uint32_t reads = owner->pipe_read_mask;
    uint32_t writes = owner->pipe_write_mask;
    owner->pipe_read_mask = 0u;
    owner->pipe_write_mask = 0u;

    for (uint32_t i = 0; i < LIONOS_PIPE_MAX; ++i) {
        uint32_t bit = 1u << i;
        struct pipe_state *p = &pipes[i];
        if ((reads & bit) && p->used && p->readers) {
            --p->readers;
            changed |= bit;
        }
        if ((writes & bit) && p->used && p->writers) {
            --p->writers;
            changed |= bit;
        }
        if (p->used && !p->readers && !p->writers) p->used = 0u;
    }
    spinlock_irqrestore_release(&pipe_lock, irq);

    for (uint32_t i = 0; i < LIONOS_PIPE_MAX; ++i) {
        if (changed & (1u << i)) {
            process_wakeup((uintptr_t)pipe_channel(i, 0u));
            process_wakeup((uintptr_t)pipe_channel(i, 1u));
        }
    }
}
