#ifndef LIONOS_GPU_SUBMISSION_H
#define LIONOS_GPU_SUBMISSION_H

#include <stdint.h>
#include "gpu_command_buffer.h"
#include "gpu_fence.h"

/*
 * CPU-only submission lifetime wrapper.
 *
 * This object leases a sealed CPU view and releases that lease only when the
 * caller declares its final CPU access complete. It does not enqueue work,
 * submit to a device, perform DMA, or prove GPU completion.
 */
typedef enum {
    GPU_SUBMISSION_INACTIVE = 0,
    GPU_SUBMISSION_ACTIVE = 1,
    GPU_SUBMISSION_COMPLETED = 2,
    GPU_SUBMISSION_BROKEN = 3
} gpu_submission_state_t;

typedef struct {
    uint32_t magic;
    gpu_command_buffer_t *buffer;
    gpu_command_buffer_view_t view;
    gpu_fence_t fence;
    gpu_submission_state_t state;
} gpu_submission_t;

/*
 * Begin a CPU consumer lifetime over an already sealed command buffer.
 * The descriptor must be zero-initialized before its first use and must not
 * be reinitialized while active or after completion.
 */
int gpu_submission_begin(gpu_submission_t *submission,
                         gpu_command_buffer_t *buffer,
                         uint32_t sequence);

/* Obtain a validated leased view while the submission is active. */
int gpu_submission_get_view(const gpu_submission_t *submission,
                            gpu_command_buffer_view_t *out_view);

/*
 * Complete after the consumer's final CPU access. The lease is released
 * before the one-shot software fence becomes terminal.
 */
int gpu_submission_complete(gpu_submission_t *submission);
int gpu_submission_fail(gpu_submission_t *submission, uint32_t error_code);
int gpu_submission_get_status(const gpu_submission_t *submission,
                              gpu_fence_status_t *out_status,
                              uint32_t *out_error_code);

/* Deterministic CPU-only submission/lease/fence lifecycle self-test. */
int gpu_submission_selftest(void);

#endif
