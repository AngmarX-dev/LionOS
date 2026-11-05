#include <stdint.h>
#include "gpu_command_buffer.h"
#include "gpu_memory.h"

#define GPU_COMMAND_DWORD_BYTES 4u

static int gpu_command_buffer_valid(const gpu_command_buffer_t *buffer)
{
    uintptr_t start;
    uintptr_t bytes;
    uintptr_t descriptor_start;
    uintptr_t descriptor_bytes = sizeof(*buffer);

    if (!buffer || !buffer->storage || !buffer->capacity_dwords ||
        buffer->capacity_dwords > UINT32_MAX / GPU_COMMAND_DWORD_BYTES ||
        buffer->used_dwords > buffer->capacity_dwords ||
        !buffer->generation ||
        (buffer->state != GPU_COMMAND_BUFFER_OPEN &&
         buffer->state != GPU_COMMAND_BUFFER_SEALED) ||
        ((uintptr_t)buffer->storage & (GPU_COMMAND_DWORD_BYTES - 1u)) != 0u)
        return 0;

    start = (uintptr_t)buffer->storage;
    bytes = (uintptr_t)buffer->capacity_dwords * GPU_COMMAND_DWORD_BYTES;
    descriptor_start = (uintptr_t)buffer;
    if (start > UINTPTR_MAX - bytes ||
        descriptor_start > UINTPTR_MAX - descriptor_bytes)
        return 0;

    if (start < descriptor_start + descriptor_bytes &&
        descriptor_start < start + bytes)
        return 0;

    return 1;
}

int gpu_command_buffer_init(gpu_command_buffer_t *buffer,
                            uint32_t *storage,
                            uint32_t capacity_dwords)
{
    gpu_command_buffer_t initialized;
    uintptr_t descriptor_start;
    uintptr_t storage_start;
    uintptr_t storage_bytes;

    if (!buffer || !storage || !capacity_dwords ||
        capacity_dwords > UINT32_MAX / GPU_COMMAND_DWORD_BYTES ||
        ((uintptr_t)storage & (GPU_COMMAND_DWORD_BYTES - 1u)) != 0u ||
        (uintptr_t)storage >
            UINTPTR_MAX - (uintptr_t)capacity_dwords * GPU_COMMAND_DWORD_BYTES)
        return -1;

    descriptor_start = (uintptr_t)buffer;
    storage_start = (uintptr_t)storage;
    storage_bytes = (uintptr_t)capacity_dwords * GPU_COMMAND_DWORD_BYTES;
    if (descriptor_start > UINTPTR_MAX - sizeof(*buffer) ||
        (storage_start < descriptor_start + sizeof(*buffer) &&
         descriptor_start < storage_start + storage_bytes))
        return -1;

    initialized.storage = storage;
    initialized.capacity_dwords = capacity_dwords;
    initialized.used_dwords = 0u;
    initialized.generation = 1u;
    initialized.state = GPU_COMMAND_BUFFER_OPEN;
    initialized.active_lease_mask = 0u;
    initialized.backing_block = (gpu_memory_block_t){0};
    initialized.backing_pinned = 0u;
    *buffer = initialized;
    return 0;
}

int gpu_command_buffer_append_u32(gpu_command_buffer_t *buffer,
                                  uint32_t value)
{
    if (!gpu_command_buffer_valid(buffer) ||
        buffer->state != GPU_COMMAND_BUFFER_OPEN ||
        buffer->used_dwords >= buffer->capacity_dwords)
        return -1;

    buffer->storage[buffer->used_dwords] = value;
    ++buffer->used_dwords;
    return 0;
}

int gpu_command_buffer_append_words(gpu_command_buffer_t *buffer,
                                    const uint32_t *words,
                                    uint32_t word_count)
{
    uintptr_t source_start;
    uintptr_t source_bytes;
    uintptr_t destination_start;
    uintptr_t destination_bytes;
    uint32_t i;

    if (!gpu_command_buffer_valid(buffer) ||
        buffer->state != GPU_COMMAND_BUFFER_OPEN ||
        (!words && word_count) ||
        word_count > buffer->capacity_dwords - buffer->used_dwords ||
        word_count > UINT32_MAX / GPU_COMMAND_DWORD_BYTES)
        return -1;

    if (!word_count)
        return 0;

    if (((uintptr_t)words & (GPU_COMMAND_DWORD_BYTES - 1u)) != 0u)
        return -1;

    source_start = (uintptr_t)words;
    source_bytes = (uintptr_t)word_count * GPU_COMMAND_DWORD_BYTES;
    destination_start = (uintptr_t)buffer->storage;
    destination_bytes =
        (uintptr_t)buffer->capacity_dwords * GPU_COMMAND_DWORD_BYTES;

    if (source_start > UINTPTR_MAX - source_bytes ||
        destination_start > UINTPTR_MAX - destination_bytes)
        return -1;

    /* Reject aliasing instead of relying on overlapping-copy semantics. */
    if (source_start < destination_start + destination_bytes &&
        destination_start < source_start + source_bytes)
        return -1;

    for (i = 0u; i < word_count; ++i)
        buffer->storage[buffer->used_dwords + i] = words[i];

    buffer->used_dwords += word_count;
    return 0;
}

int gpu_command_buffer_seal(gpu_command_buffer_t *buffer)
{
    if (!gpu_command_buffer_valid(buffer) ||
        buffer->state != GPU_COMMAND_BUFFER_OPEN)
        return -1;

    buffer->state = GPU_COMMAND_BUFFER_SEALED;
    return 0;
}

int gpu_command_buffer_reset(gpu_command_buffer_t *buffer)
{
    if (!gpu_command_buffer_valid(buffer) ||
        buffer->active_lease_mask != 0u ||
        buffer->generation == UINT32_MAX)
        return -1;

    ++buffer->generation;
    buffer->used_dwords = 0u;
    buffer->state = GPU_COMMAND_BUFFER_OPEN;
    return 0;
}

static int gpu_command_ranges_overlap(uintptr_t first,
                                     uintptr_t first_size,
                                     uintptr_t second,
                                     uintptr_t second_size)
{
    if (first > UINTPTR_MAX - first_size ||
        second > UINTPTR_MAX - second_size)
        return 1;

    return first < second + second_size &&
           second < first + first_size;
}

int gpu_command_buffer_get_view(const gpu_command_buffer_t *buffer,
                                gpu_command_buffer_view_t *out_view)
{
    gpu_command_buffer_view_t view;
    uintptr_t output_start;
    uintptr_t descriptor_start;
    uintptr_t storage_start;
    uintptr_t storage_bytes;

    if (!gpu_command_buffer_valid(buffer) || !out_view ||
        buffer->state != GPU_COMMAND_BUFFER_SEALED)
        return -1;

    output_start = (uintptr_t)out_view;
    descriptor_start = (uintptr_t)buffer;
    storage_start = (uintptr_t)buffer->storage;
    storage_bytes = (uintptr_t)buffer->capacity_dwords *
                    GPU_COMMAND_DWORD_BYTES;

    if (gpu_command_ranges_overlap(output_start, sizeof(*out_view),
                                   descriptor_start, sizeof(*buffer)) ||
        gpu_command_ranges_overlap(output_start, sizeof(*out_view),
                                   storage_start, storage_bytes))
        return -1;

    view.words = buffer->storage;
    view.word_count = buffer->used_dwords;
    view.generation = buffer->generation;
    view.lease_token = 0u;
    *out_view = view;
    return 0;
}

int gpu_command_buffer_init_from_memory(gpu_command_buffer_t *buffer,
                                        const gpu_memory_block_t *backing)
{
    uint64_t physical_address;
    uint32_t cpu_virtual_address;

    if (!buffer || !backing ||
        gpu_command_ranges_overlap((uintptr_t)buffer, sizeof(*buffer),
                                   (uintptr_t)backing, sizeof(*backing)) ||
        backing->page_count != 1u || backing->size_bytes != 4096u ||
        backing->address_space_reserved != 0u ||
        gpu_memory_get_page(backing, 0u, &physical_address,
                            &cpu_virtual_address) != 0 ||
        !cpu_virtual_address ||
        (physical_address & 0xFFFu) != 0u ||
        (cpu_virtual_address & 0xFFFu) != 0u)
        return -1;

    if (gpu_memory_pin(backing) != 0)
        return -1;
    if (gpu_command_buffer_init(buffer,
            (uint32_t *)(uintptr_t)cpu_virtual_address,
            4096u / sizeof(uint32_t)) != 0) {
        (void)gpu_memory_unpin(backing);
        return -1;
    }
    buffer->backing_block = *backing;
    buffer->backing_pinned = 1u;
    return 0;
}

int gpu_command_buffer_acquire_view(gpu_command_buffer_t *buffer,
                                    gpu_command_buffer_view_t *out_view)
{
    gpu_command_buffer_view_t view;
    uint32_t available;
    uint32_t token;
    if (!gpu_command_buffer_valid(buffer) || !out_view ||
        buffer->state != GPU_COMMAND_BUFFER_SEALED ||
        buffer->active_lease_mask == UINT32_MAX)
        return -1;
    available = ~buffer->active_lease_mask;
    token = available & (0u - available);
    if (!token || gpu_command_buffer_get_view(buffer, &view) != 0)
        return -1;
    view.lease_token = token;
    buffer->active_lease_mask |= token;
    *out_view = view;
    return 0;
}

int gpu_command_buffer_release_view(gpu_command_buffer_t *buffer,
                                    const gpu_command_buffer_view_t *view)
{
    uint32_t token;
    if (!gpu_command_buffer_valid(buffer) || !view)
        return -1;
    token = view->lease_token;
    if (!token || (token & (token - 1u)) != 0u ||
        (buffer->active_lease_mask & token) == 0u ||
        buffer->state != GPU_COMMAND_BUFFER_SEALED ||
        view->words != buffer->storage ||
        view->word_count != buffer->used_dwords ||
        view->generation != buffer->generation)
        return -1;
    buffer->active_lease_mask &= ~token;
    return 0;
}

int gpu_command_buffer_destroy(gpu_command_buffer_t *buffer)
{
    if (!gpu_command_buffer_valid(buffer) ||
        buffer->active_lease_mask != 0u || buffer->backing_pinned > 1u)
        return -1;
    if (buffer->backing_pinned &&
        gpu_memory_unpin(&buffer->backing_block) != 0)
        return -1;
    *buffer = (gpu_command_buffer_t){0};
    return 0;
}

int gpu_command_buffer_view_validate(const gpu_command_buffer_t *buffer,
                                     const gpu_command_buffer_view_t *view)
{
    uintptr_t view_start;
    uintptr_t descriptor_start;
    uintptr_t storage_start;
    uintptr_t storage_bytes;

    if (!gpu_command_buffer_valid(buffer) || !view ||
        buffer->state != GPU_COMMAND_BUFFER_SEALED)
        return -1;

    view_start = (uintptr_t)view;
    descriptor_start = (uintptr_t)buffer;
    storage_start = (uintptr_t)buffer->storage;
    storage_bytes = (uintptr_t)buffer->capacity_dwords *
                    GPU_COMMAND_DWORD_BYTES;

    if (gpu_command_ranges_overlap(view_start, sizeof(*view),
                                   descriptor_start, sizeof(*buffer)) ||
        gpu_command_ranges_overlap(view_start, sizeof(*view),
                                   storage_start, storage_bytes))
        return -1;

    if (view->words != buffer->storage ||
        view->word_count != buffer->used_dwords ||
        view->generation != buffer->generation ||
        (view->lease_token != 0u &&
         ((view->lease_token & (view->lease_token - 1u)) != 0u ||
          (buffer->active_lease_mask & view->lease_token) == 0u)))
        return -1;

    return 0;
}

int gpu_command_buffer_selftest(void)
{
    uint32_t guarded_storage[6] = {
        0x13579BDFu, 0u, 0u, 0u, 0u, 0x2468ACE0u
    };
    const uint32_t input[2] = {0x10203040u, 0x50607080u};
    gpu_command_buffer_t buffer = {0};
    gpu_command_buffer_t alias_buffer = {0};
    gpu_command_buffer_view_t view = {input, 77u, 0u, 0u};
    gpu_command_buffer_view_t stale_view = {0, 0u, 0u, 0u};

    if (gpu_command_buffer_init(&buffer, &guarded_storage[1], 4u) != 0 ||
        buffer.state != GPU_COMMAND_BUFFER_OPEN ||
        buffer.used_dwords != 0u || buffer.generation != 1u)
        return -1;

    if (gpu_command_buffer_append_u32(&buffer, 0xAABBCCDDu) != 0 ||
        gpu_command_buffer_append_words(&buffer, input, 2u) != 0 ||
        buffer.used_dwords != 3u ||
        guarded_storage[1] != 0xAABBCCDDu ||
        guarded_storage[2] != input[0] ||
        guarded_storage[3] != input[1] ||
        gpu_command_buffer_append_words(&buffer, input, 2u) == 0 ||
        buffer.used_dwords != 3u ||
        gpu_command_buffer_append_words(&buffer, &guarded_storage[1], 1u) == 0)
        return -1;

    /* An OPEN buffer cannot expose a view or modify the caller's descriptor. */
    if (gpu_command_buffer_get_view(&buffer, &view) == 0 ||
        view.words != input || view.word_count != 77u || view.generation != 0u)
        return -1;

    if (gpu_command_buffer_seal(&buffer) != 0 ||
        buffer.state != GPU_COMMAND_BUFFER_SEALED ||
        gpu_command_buffer_get_view(&buffer, &view) != 0 ||
        view.words != &guarded_storage[1] || view.word_count != 3u ||
        view.generation != buffer.generation ||
        gpu_command_buffer_view_validate(&buffer, &view) != 0)
        return -1;

    {
        gpu_command_buffer_view_t lease_a = {0};
        gpu_command_buffer_view_t lease_b = {0};
        if (gpu_command_buffer_acquire_view(&buffer, &lease_a) != 0 ||
            gpu_command_buffer_acquire_view(&buffer, &lease_b) != 0 ||
            lease_a.lease_token == lease_b.lease_token ||
            gpu_command_buffer_reset(&buffer) == 0 ||
            gpu_command_buffer_release_view(&buffer, &lease_a) != 0 ||
            gpu_command_buffer_release_view(&buffer, &lease_a) == 0 ||
            gpu_command_buffer_reset(&buffer) == 0 ||
            gpu_command_buffer_release_view(&buffer, &lease_b) != 0)
            return -1;
    }

    if (gpu_command_buffer_get_view(&buffer,
            (gpu_command_buffer_view_t *)&buffer) == 0 ||
        gpu_command_buffer_get_view(&buffer,
            (gpu_command_buffer_view_t *)&guarded_storage[1]) == 0 ||
        gpu_command_buffer_view_validate(&buffer,
            (const gpu_command_buffer_view_t *)&buffer) == 0 ||
        gpu_command_buffer_append_u32(&buffer, 0u) == 0 ||
        gpu_command_buffer_seal(&buffer) == 0)
        return -1;

    stale_view = view;
    {
        gpu_command_buffer_view_t lease = {0};
        if (gpu_command_buffer_acquire_view(&buffer, &lease) != 0 ||
            gpu_command_buffer_reset(&buffer) == 0 ||
            gpu_command_buffer_release_view(&buffer, &lease) != 0)
            goto cleanup;
    }
    if (gpu_command_buffer_reset(&buffer) != 0 ||
        buffer.state != GPU_COMMAND_BUFFER_OPEN ||
        buffer.used_dwords != 0u || buffer.generation != 2u ||
        gpu_command_buffer_get_view(&buffer, &view) == 0 ||
        view.words != stale_view.words ||
        view.word_count != stale_view.word_count ||
        view.generation != stale_view.generation ||
        gpu_command_buffer_view_validate(&buffer, &stale_view) == 0 ||
        gpu_command_buffer_append_words(&buffer, 0, 0u) != 0)
        return -1;

    /* Reuse the same storage with the same count: the old generation stays stale. */
    if (gpu_command_buffer_append_u32(&buffer, 0xAABBCCDDu) != 0 ||
        gpu_command_buffer_append_words(&buffer, input, 2u) != 0 ||
        gpu_command_buffer_seal(&buffer) != 0 ||
        gpu_command_buffer_view_validate(&buffer, &stale_view) == 0 ||
        gpu_command_buffer_get_view(&buffer, &view) != 0 ||
        gpu_command_buffer_view_validate(&buffer, &view) != 0 ||
        view.words != stale_view.words ||
        view.word_count != stale_view.word_count ||
        view.generation == stale_view.generation)
        return -1;

    /* Generation exhaustion must fail closed instead of wrapping. */
    buffer.generation = UINT32_MAX;
    if (gpu_command_buffer_reset(&buffer) == 0 ||
        buffer.generation != UINT32_MAX ||
        buffer.state != GPU_COMMAND_BUFFER_SEALED)
        return -1;

    if (gpu_command_buffer_destroy(&buffer) != 0 ||
        gpu_command_buffer_destroy(&buffer) == 0 ||
        gpu_command_buffer_init(&alias_buffer,
            (uint32_t *)&alias_buffer, 4u) == 0 ||
        guarded_storage[0] != 0x13579BDFu ||
        guarded_storage[5] != 0x2468ACE0u)
        return -1;

    return 0;
}

int gpu_command_buffer_memory_selftest(gfx_backend_t backend)
{
    gpu_memory_block_t backing = {0};
    gpu_command_buffer_t buffer = {0};
    gpu_command_buffer_view_t view = {0, 0u, 0u, 0u};
    gpu_command_buffer_view_t stale_view = {0, 0u, 0u, 0u};
    uint64_t physical_address = 0u;
    uint32_t cpu_virtual_address = 0u;
    int result = -1;
    int buffer_initialized = 0;

    if (gpu_memory_alloc(backend, 1u, &backing) != 0)
        return -1;

    if (backing.page_count != 1u || backing.size_bytes != 4096u ||
        backing.address_space_reserved != 0u ||
        gpu_memory_get_page(&backing, 0u, &physical_address,
                            &cpu_virtual_address) != 0 ||
        !cpu_virtual_address ||
        (physical_address & 0xFFFu) != 0u ||
        (cpu_virtual_address & 0xFFFu) != 0u ||
        gpu_command_buffer_init_from_memory(&buffer, &backing) != 0)
        goto cleanup;
    buffer_initialized = 1;

    if (buffer.storage != (uint32_t *)(uintptr_t)cpu_virtual_address ||
        buffer.capacity_dwords != 1024u ||
        gpu_memory_free(&backing) == 0 ||
        gpu_command_buffer_append_u32(&buffer, 0xC001D00Du) != 0 ||
        gpu_command_buffer_append_u32(&buffer, 0x51A1E5EDu) != 0 ||
        gpu_command_buffer_seal(&buffer) != 0 ||
        gpu_command_buffer_get_view(&buffer, &view) != 0 ||
        view.word_count != 2u || view.words[0] != 0xC001D00Du ||
        view.words[1] != 0x51A1E5EDu ||
        gpu_command_buffer_view_validate(&buffer, &view) != 0)
        goto cleanup;

    stale_view = view;
    if (gpu_command_buffer_reset(&buffer) != 0 ||
        gpu_command_buffer_view_validate(&buffer, &stale_view) == 0 ||
        gpu_command_buffer_append_u32(&buffer, 0xC001D00Du) != 0 ||
        gpu_command_buffer_append_u32(&buffer, 0x51A1E5EDu) != 0 ||
        gpu_command_buffer_seal(&buffer) != 0 ||
        gpu_command_buffer_view_validate(&buffer, &stale_view) == 0 ||
        gpu_command_buffer_get_view(&buffer, &view) != 0 ||
        gpu_command_buffer_view_validate(&buffer, &view) != 0)
        goto cleanup;

    result = 0;

cleanup:
    if (buffer_initialized && gpu_command_buffer_destroy(&buffer) != 0)
        result = -1;
    if (gpu_memory_free(&backing) != 0)
        return -1;
    return result;
}
