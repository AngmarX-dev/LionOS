#include <stdint.h>
#include "gpu_submission.h"

#define GPU_SUBMISSION_MAGIC 0x47535542u

static int gpu_submission_ranges_overlap(uintptr_t first,
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

static int gpu_submission_valid_active(const gpu_submission_t *submission)
{
    if (!submission ||
        submission->magic != GPU_SUBMISSION_MAGIC ||
        submission->state != GPU_SUBMISSION_ACTIVE ||
        !submission->buffer ||
        gpu_command_buffer_view_validate(submission->buffer,
                                         &submission->view) != 0)
        return 0;

    return 1;
}

int gpu_submission_begin(gpu_submission_t *submission,
                         gpu_command_buffer_t *buffer,
                         uint32_t sequence)
{
    gpu_submission_t initialized = {0};
    gpu_command_buffer_view_t probe = {0};

    if (!submission || !buffer || !sequence ||
        submission->magic == GPU_SUBMISSION_MAGIC ||
        gpu_command_buffer_get_view(buffer, &probe) != 0 ||
        gpu_submission_ranges_overlap((uintptr_t)submission,
                                      sizeof(*submission),
                                      (uintptr_t)buffer, sizeof(*buffer)) ||
        gpu_submission_ranges_overlap((uintptr_t)submission,
                                      sizeof(*submission),
                                      (uintptr_t)probe.words,
                                      (uintptr_t)buffer->capacity_dwords *
                                          sizeof(uint32_t)))
        return -1;

    if (gpu_fence_init(&initialized.fence, sequence) != 0 ||
        gpu_command_buffer_acquire_view(buffer, &initialized.view) != 0)
        return -1;

    initialized.magic = GPU_SUBMISSION_MAGIC;
    initialized.buffer = buffer;
    initialized.state = GPU_SUBMISSION_ACTIVE;
    *submission = initialized;
    return 0;
}

int gpu_submission_get_view(const gpu_submission_t *submission,
                            gpu_command_buffer_view_t *out_view)
{
    if (!gpu_submission_valid_active(submission) || !out_view ||
        gpu_submission_ranges_overlap((uintptr_t)out_view, sizeof(*out_view),
                                      (uintptr_t)submission,
                                      sizeof(*submission)) ||
        gpu_submission_ranges_overlap((uintptr_t)out_view, sizeof(*out_view),
                                      (uintptr_t)submission->buffer,
                                      sizeof(*submission->buffer)) ||
        gpu_submission_ranges_overlap((uintptr_t)out_view, sizeof(*out_view),
                                      (uintptr_t)submission->view.words,
                                      (uintptr_t)submission->buffer->capacity_dwords *
                                          sizeof(uint32_t)))
        return -1;

    *out_view = submission->view;
    return 0;
}

static int gpu_submission_finish(gpu_submission_t *submission,
                                 uint32_t error_code)
{
    if (!gpu_submission_valid_active(submission))
        return -1;

    /* No consumer may access the view after this call begins. */
    if (gpu_command_buffer_release_view(submission->buffer,
                                        &submission->view) != 0)
        return -1;

    if (error_code) {
        if (gpu_fence_fail(&submission->fence, error_code) != 0) {
            submission->state = GPU_SUBMISSION_BROKEN;
            return -1;
        }
    } else if (gpu_fence_signal(&submission->fence) != 0) {
        submission->state = GPU_SUBMISSION_BROKEN;
        return -1;
    }

    submission->state = GPU_SUBMISSION_COMPLETED;
    return 0;
}

int gpu_submission_complete(gpu_submission_t *submission)
{
    return gpu_submission_finish(submission, 0u);
}

int gpu_submission_fail(gpu_submission_t *submission, uint32_t error_code)
{
    if (!error_code)
        return -1;
    return gpu_submission_finish(submission, error_code);
}

int gpu_submission_get_status(const gpu_submission_t *submission,
                              gpu_fence_status_t *out_status,
                              uint32_t *out_error_code)
{
    if (!submission || submission->magic != GPU_SUBMISSION_MAGIC ||
        !out_status || !out_error_code ||
        (submission->state != GPU_SUBMISSION_ACTIVE &&
         submission->state != GPU_SUBMISSION_COMPLETED &&
         submission->state != GPU_SUBMISSION_BROKEN) ||
        gpu_submission_ranges_overlap((uintptr_t)out_status,
                                      sizeof(*out_status),
                                      (uintptr_t)submission,
                                      sizeof(*submission)) ||
        gpu_submission_ranges_overlap((uintptr_t)out_error_code,
                                      sizeof(*out_error_code),
                                      (uintptr_t)submission,
                                      sizeof(*submission)))
        return -1;

    return gpu_fence_get_status(&submission->fence,
                                out_status, out_error_code);
}

int gpu_submission_selftest(void)
{
    uint32_t storage[8] = {0};
    gpu_command_buffer_t buffer = {0};
    gpu_submission_t success = {0};
    gpu_submission_t failure = {0};
    gpu_command_buffer_view_t view = {0};
    gpu_fence_status_t status = GPU_FENCE_STATUS_FAILED;
    uint32_t error_code = 0xA5A5A5A5u;
    int result = -1;

    if (gpu_command_buffer_init(&buffer, storage, 8u) != 0 ||
        gpu_command_buffer_append_u32(&buffer, 0xC001D00Du) != 0 ||
        gpu_command_buffer_append_u32(&buffer, 0x51A1E5EDu) != 0 ||
        gpu_command_buffer_seal(&buffer) != 0)
        goto cleanup;

    if (gpu_submission_begin(&success, &buffer, 1u) != 0 ||
        gpu_submission_begin(&success, &buffer, 99u) == 0 ||
        gpu_submission_get_view(&success, &view) != 0 ||
        gpu_submission_get_view(&success,
            (gpu_command_buffer_view_t *)&success) == 0 ||
        gpu_submission_get_view(&success,
            (gpu_command_buffer_view_t *)storage) == 0 ||
        gpu_submission_get_status(&success, &status,
            (uint32_t *)&success) == 0 ||
        view.word_count != 2u || view.words[0] != 0xC001D00Du ||
        view.words[1] != 0x51A1E5EDu ||
        gpu_command_buffer_reset(&buffer) == 0 ||
        gpu_submission_get_status(&success, &status, &error_code) != 0 ||
        status != GPU_FENCE_STATUS_PENDING || error_code != 0u ||
        gpu_submission_complete(&success) != 0 ||
        gpu_submission_complete(&success) == 0 ||
        gpu_submission_get_status(&success, &status, &error_code) != 0 ||
        status != GPU_FENCE_STATUS_SIGNALED || error_code != 0u ||
        gpu_command_buffer_reset(&buffer) != 0)
        goto cleanup;

    if (gpu_command_buffer_append_u32(&buffer, 0xDEADBEEFu) != 0 ||
        gpu_command_buffer_seal(&buffer) != 0 ||
        gpu_submission_begin(&failure, &buffer, 2u) != 0 ||
        gpu_submission_get_view(&failure, &view) != 0 ||
        view.word_count != 1u || view.words[0] != 0xDEADBEEFu ||
        gpu_submission_fail(&failure, 0u) == 0 ||
        gpu_submission_fail(&failure, 0xE101u) != 0 ||
        gpu_submission_get_status(&failure, &status, &error_code) != 0 ||
        status != GPU_FENCE_STATUS_FAILED || error_code != 0xE101u ||
        gpu_command_buffer_reset(&buffer) != 0)
        goto cleanup;

    result = 0;

cleanup:
    if (success.state == GPU_SUBMISSION_ACTIVE)
        (void)gpu_submission_fail(&success, 0xE1FFu);
    if (failure.state == GPU_SUBMISSION_ACTIVE)
        (void)gpu_submission_fail(&failure, 0xE1FEu);
    if (buffer.state != GPU_COMMAND_BUFFER_UNINITIALIZED) {
        if (buffer.active_lease_mask != 0u)
            result = -1;
        if (buffer.state == GPU_COMMAND_BUFFER_SEALED &&
            gpu_command_buffer_reset(&buffer) != 0)
            result = -1;
        if (gpu_command_buffer_destroy(&buffer) != 0)
            result = -1;
    }
    return result;
}
