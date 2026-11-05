#include <stdint.h>
#include "gpu_memory.h"
#include "intel_gpu.h"

static int gpu_memory_backend_supported(gfx_backend_t backend)
{
    return backend == GFX_BACKEND_INTEL_GPU;
}

int gpu_memory_init(gfx_backend_t backend)
{
    if (!gpu_memory_backend_supported(backend) ||
        gfx_backend_get_state(backend) != GFX_BACKEND_STATE_INITIALIZED)
        return -1;

    return intel_gpu_memory_init();
}

int gpu_memory_alloc(gfx_backend_t backend, uint32_t page_count,
                     gpu_memory_block_t *out)
{
    intel_gpu_memory_block_t provider_block = {0};
    gpu_memory_block_t result;

    if (!out || !gpu_memory_backend_supported(backend) ||
        gfx_backend_get_state(backend) != GFX_BACKEND_STATE_INITIALIZED ||
        intel_gpu_memory_alloc(page_count, &provider_block) != 0)
        return -1;

    result.backend = backend;
    result.provider_handle = provider_block.handle;
    result.page_count = provider_block.page_count;
    result.size_bytes = provider_block.bytes;
    result.address_space_first = provider_block.ggtt_first_entry;
    result.address_space_reserved =
        provider_block.ggtt_first_entry != UINT32_MAX ? 1u : 0u;

    *out = result;
    return 0;
}

static int gpu_memory_block_valid(const gpu_memory_block_t *block)
{
    if (!block || !block->provider_handle || !block->page_count ||
        block->page_count > UINT32_MAX / 4096u ||
        block->size_bytes != block->page_count * 4096u ||
        !gpu_memory_backend_supported(block->backend) ||
        block->address_space_reserved > 1u)
        return 0;

    if (block->address_space_reserved)
        return block->address_space_first != 0u &&
               block->address_space_first != UINT32_MAX;
    return block->address_space_first == UINT32_MAX;
}

int gpu_memory_get_page(const gpu_memory_block_t *block, uint32_t page_index,
                        uint64_t *physical_address,
                        uint32_t *cpu_virtual_address)
{
    uint64_t physical;
    uint32_t virtual_address;

    if (!gpu_memory_block_valid(block) || !physical_address ||
        !cpu_virtual_address || page_index >= block->page_count ||
        intel_gpu_memory_get_page(block->provider_handle, page_index,
                                  &physical, &virtual_address) != 0)
        return -1;

    *physical_address = physical;
    *cpu_virtual_address = virtual_address;
    return 0;
}

int gpu_memory_get_mapping_candidate(const gpu_memory_block_t *block,
                                     uint32_t page_index,
                                     uint32_t *entry_index,
                                     uint64_t *pte_value)
{
    uint32_t index;
    uint64_t pte;

    if (!gpu_memory_block_valid(block) || !entry_index || !pte_value ||
        !block->address_space_reserved || page_index >= block->page_count ||
        block->address_space_first > UINT32_MAX - page_index ||
        intel_gpu_memory_get_mapping_candidate(block->provider_handle,
                                               page_index, &index, &pte) != 0 ||
        index != block->address_space_first + page_index)
        return -1;

    *entry_index = index;
    *pte_value = pte;
    return 0;
}

int gpu_memory_free(gpu_memory_block_t *block)
{
    if (!gpu_memory_block_valid(block) ||
        intel_gpu_memory_free(block->provider_handle) != 0)
        return -1;

    *block = (gpu_memory_block_t){0};
    return 0;
}

int gpu_memory_get_stats(gfx_backend_t backend, gpu_memory_stats_t *out)
{
    intel_gpu_memory_stats_t provider_stats;

    if (!out || !gpu_memory_backend_supported(backend) ||
        intel_gpu_memory_get_stats(&provider_stats) != 0)
        return -1;

    out->backend = backend;
    out->total_address_space_entries = provider_stats.ggtt_total_entries;
    out->empty_address_space_entries = provider_stats.ggtt_empty_candidates;
    out->valid_address_space_entries = provider_stats.ggtt_valid_entries;
    out->nonzero_invalid_address_space_entries =
        provider_stats.ggtt_nonzero_invalid_entries;
    out->active_allocations = provider_stats.active_allocations;
    out->allocated_pages = provider_stats.allocated_pages;
    out->initialized = provider_stats.initialized;
    out->address_space_ownership_verified =
        provider_stats.ggtt_ownership_verified;
    return 0;
}

int gpu_memory_selftest(gfx_backend_t backend)
{
    gpu_memory_block_t block = {0};
    gpu_memory_stats_t before = {0}, during = {0}, after = {0};
    uint64_t physical = 0u;
    uint32_t virtual_address = 0u;
    int result = -1;

    if (!gpu_memory_backend_supported(backend) ||
        gpu_memory_get_stats(backend, &before) != 0 ||
        !before.initialized ||
        before.address_space_ownership_verified != 0u ||
        gpu_memory_alloc(backend, 1u, &block) != 0)
        return -1;

    if (block.backend != backend || block.page_count != 1u ||
        block.size_bytes != 4096u || block.address_space_reserved != 0u ||
        block.address_space_first != UINT32_MAX ||
        gpu_memory_get_stats(backend, &during) != 0 ||
        during.address_space_ownership_verified != 0u ||
        during.active_allocations != before.active_allocations + 1u ||
        during.allocated_pages != before.allocated_pages + 1u ||
        gpu_memory_get_page(&block, 0u, &physical, &virtual_address) != 0 ||
        (physical & 0xFFFu) != 0u || (virtual_address & 0xFFFu) != 0u)
        goto cleanup;

    if (block.address_space_reserved) {
        uint32_t entry = UINT32_MAX;
        uint64_t pte = 0u;
        if (gpu_memory_get_mapping_candidate(&block, 0u, &entry, &pte) != 0 ||
            entry != block.address_space_first || (pte & 1u) == 0u)
            goto cleanup;
    } else {
        uint32_t entry = 0xA5A5A5A5u;
        uint64_t pte = 0xA5A5A5A5A5A5A5A5ULL;
        if (gpu_memory_get_mapping_candidate(&block, 0u, &entry, &pte) == 0 ||
            entry != 0xA5A5A5A5u ||
            pte != 0xA5A5A5A5A5A5A5A5ULL)
            goto cleanup;
    }

    result = 0;

cleanup:
    if (gpu_memory_free(&block) != 0 ||
        block.provider_handle != 0u || block.page_count != 0u ||
        block.size_bytes != 0u)
        return -1;
    if (gpu_memory_get_stats(backend, &after) != 0 ||
        after.address_space_ownership_verified != 0u ||
        after.active_allocations != before.active_allocations ||
        after.allocated_pages != before.allocated_pages)
        return -1;
    return result;
}
