#ifndef LIONOS_GPU_FENCE_H
#define LIONOS_GPU_FENCE_H

#include <stdint.h>

/*
 * One-shot, CPU-visible completion primitive for future GPU submissions.
 *
 * A fence is initialized before publication and must remain alive while any
 * producer or observer can access it. It is not a hardware fence, DMA fence,
 * interrupt handler, scheduler wait queue, or GPU completion proof.
 */
typedef enum {
    GPU_FENCE_STATUS_PENDING = 0,
    GPU_FENCE_STATUS_SIGNALED = 1,
    GPU_FENCE_STATUS_FAILED = 2
} gpu_fence_status_t;

typedef struct {
    volatile uint32_t state;
    uint32_t sequence;
    uint32_t error_code;
    uint32_t initialized;
} gpu_fence_t;

/* Sequence zero is reserved and rejected. Fences are one-shot; no reset API. */
int gpu_fence_init(gpu_fence_t *fence, uint32_t sequence);

/* Exactly one producer may complete a pending fence. error_code must be nonzero. */
int gpu_fence_signal(gpu_fence_t *fence);
int gpu_fence_fail(gpu_fence_t *fence, uint32_t error_code);

/*
 * Non-blocking status query. Returns 0 on success, -1 on invalid arguments.
 * Outputs are written only on success. COMPLETING is reported as PENDING.
 */
int gpu_fence_get_status(const gpu_fence_t *fence,
                         gpu_fence_status_t *out_status,
                         uint32_t *out_error_code);

/* Deterministic CPU-only lifecycle and publication-order self-test. */
int gpu_fence_selftest(void);

#endif
