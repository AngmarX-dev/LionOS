#include <stdint.h>
#include "gpu_fence.h"

#define GPU_FENCE_INITIALIZED_MAGIC 0x47464E43u

#define GPU_FENCE_INTERNAL_PENDING    0u
#define GPU_FENCE_INTERNAL_COMPLETING 1u
#define GPU_FENCE_INTERNAL_SIGNALED   2u
#define GPU_FENCE_INTERNAL_FAILED    3u

static int gpu_fence_ranges_overlap(uintptr_t first,
                                    uintptr_t first_size,
                                    uintptr_t second,
                                    uintptr_t second_size)
{
    if (!first_size || !second_size ||
        first > UINTPTR_MAX - first_size ||
        second > UINTPTR_MAX - second_size)
        return 1;

    return first < second + second_size &&
           second < first + first_size;
}

static int gpu_fence_valid(const gpu_fence_t *fence)
{
    uint32_t state;

    if (!fence ||
        fence->initialized != GPU_FENCE_INITIALIZED_MAGIC ||
        !fence->sequence)
        return 0;

    state = __atomic_load_n(&fence->state, __ATOMIC_ACQUIRE);
    return state <= GPU_FENCE_INTERNAL_FAILED;
}

int gpu_fence_init(gpu_fence_t *fence, uint32_t sequence)
{
    gpu_fence_t initialized;

    if (!fence || !sequence)
        return -1;

    initialized.state = GPU_FENCE_INTERNAL_PENDING;
    initialized.sequence = sequence;
    initialized.error_code = 0u;
    initialized.initialized = GPU_FENCE_INITIALIZED_MAGIC;
    *fence = initialized;
    return 0;
}

static int gpu_fence_complete(gpu_fence_t *fence,
                              uint32_t terminal_state,
                              uint32_t error_code)
{
    uint32_t expected = GPU_FENCE_INTERNAL_PENDING;

    if (!gpu_fence_valid(fence) ||
        (terminal_state != GPU_FENCE_INTERNAL_SIGNALED &&
         terminal_state != GPU_FENCE_INTERNAL_FAILED) ||
        ((terminal_state == GPU_FENCE_INTERNAL_FAILED) !=
         (error_code != 0u)))
        return -1;

    /*
     * The intermediate state elects one producer. Observers treat it as
     * pending until error_code is published with the terminal release store.
     */
    if (!__atomic_compare_exchange_n(&fence->state, &expected,
                                    GPU_FENCE_INTERNAL_COMPLETING, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return -1;

    fence->error_code = error_code;
    __atomic_store_n(&fence->state, terminal_state, __ATOMIC_RELEASE);
    return 0;
}

int gpu_fence_signal(gpu_fence_t *fence)
{
    return gpu_fence_complete(fence, GPU_FENCE_INTERNAL_SIGNALED, 0u);
}

int gpu_fence_fail(gpu_fence_t *fence, uint32_t error_code)
{
    if (!error_code)
        return -1;

    return gpu_fence_complete(fence, GPU_FENCE_INTERNAL_FAILED, error_code);
}

int gpu_fence_get_status(const gpu_fence_t *fence,
                         gpu_fence_status_t *out_status,
                         uint32_t *out_error_code)
{
    gpu_fence_status_t status;
    uint32_t error_code;
    uint32_t state;
    uintptr_t fence_start;
    uintptr_t status_start;
    uintptr_t error_start;

    if (!gpu_fence_valid(fence) || !out_status || !out_error_code)
        return -1;

    fence_start = (uintptr_t)fence;
    status_start = (uintptr_t)out_status;
    error_start = (uintptr_t)out_error_code;

    if (gpu_fence_ranges_overlap(status_start, sizeof(*out_status),
                                fence_start, sizeof(*fence)) ||
        gpu_fence_ranges_overlap(error_start, sizeof(*out_error_code),
                                fence_start, sizeof(*fence)) ||
        gpu_fence_ranges_overlap(status_start, sizeof(*out_status),
                                error_start, sizeof(*out_error_code)))
        return -1;

    state = __atomic_load_n(&fence->state, __ATOMIC_ACQUIRE);
    error_code = 0u;

    switch (state) {
    case GPU_FENCE_INTERNAL_PENDING:
    case GPU_FENCE_INTERNAL_COMPLETING:
        status = GPU_FENCE_STATUS_PENDING;
        break;
    case GPU_FENCE_INTERNAL_SIGNALED:
        status = GPU_FENCE_STATUS_SIGNALED;
        break;
    case GPU_FENCE_INTERNAL_FAILED:
        status = GPU_FENCE_STATUS_FAILED;
        error_code = fence->error_code;
        if (!error_code)
            return -1;
        break;
    default:
        return -1;
    }

    *out_status = status;
    *out_error_code = error_code;
    return 0;
}

int gpu_fence_selftest(void)
{
    gpu_fence_t signaled = {0};
    gpu_fence_t failed = {0};
    gpu_fence_t untouched = {0};
    gpu_fence_status_t status = GPU_FENCE_STATUS_FAILED;
    uint32_t error_code = 0xA5A5A5A5u;

    if (gpu_fence_init(&signaled, 1u) != 0 ||
        gpu_fence_get_status(&signaled, &status, &error_code) != 0 ||
        status != GPU_FENCE_STATUS_PENDING || error_code != 0u ||
        gpu_fence_signal(&signaled) != 0 ||
        gpu_fence_get_status(&signaled, &status, &error_code) != 0 ||
        status != GPU_FENCE_STATUS_SIGNALED || error_code != 0u ||
        gpu_fence_signal(&signaled) == 0 ||
        gpu_fence_fail(&signaled, 1u) == 0)
        return -1;

    if (gpu_fence_init(&failed, 2u) != 0 ||
        gpu_fence_fail(&failed, 0u) == 0 ||
        gpu_fence_fail(&failed, 0xE001u) != 0 ||
        gpu_fence_get_status(&failed, &status, &error_code) != 0 ||
        status != GPU_FENCE_STATUS_FAILED || error_code != 0xE001u ||
        gpu_fence_signal(&failed) == 0 ||
        gpu_fence_fail(&failed, 0xE002u) == 0)
        return -1;

    if (gpu_fence_init(&untouched, 0u) == 0 ||
        gpu_fence_init(0, 3u) == 0 ||
        gpu_fence_get_status(&signaled, &status, &status) == 0 ||
        gpu_fence_get_status(&signaled, &status,
                             (uint32_t *)&signaled) == 0)
        return -1;

    /* Uninitialized descriptors and invalid queries must preserve outputs. */
    status = GPU_FENCE_STATUS_FAILED;
    error_code = 0xA5A5A5A5u;
    if (gpu_fence_get_status(&untouched, &status, &error_code) == 0 ||
        gpu_fence_get_status(0, &status, &error_code) == 0 ||
        status != GPU_FENCE_STATUS_FAILED ||
        error_code != 0xA5A5A5A5u)
        return -1;

    return 0;
}
