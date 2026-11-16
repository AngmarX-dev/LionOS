#ifndef LIONOS_GPU_MEMORY_H
#define LIONOS_GPU_MEMORY_H

#include <stdint.h>
#include "gfx.h"

/*
 * Vendor-neutral GPU backing-store interface.
 *
 * Blocks own CPU-accessible system RAM. GGTT zero entries are not considered
 * owned by LionOS, so address_space_reserved remains false. This API does not
 * program GPU page tables or make a block GPU-addressable. All operations are
 * BSP-only until the memory subsystem provides a serialized multi-CPU contract.
 */
typedef struct {
    gfx_backend_t backend;
    uint32_t provider_handle;
    uint32_t page_count;
    uint32_t size_bytes;
    uint32_t address_space_first;
    uint32_t address_space_reserved;
} gpu_memory_block_t;

typedef struct {
    gfx_backend_t backend;
    uint32_t total_address_space_entries;
    uint32_t empty_address_space_entries;
    uint32_t valid_address_space_entries;
    uint32_t nonzero_invalid_address_space_entries;
    uint32_t active_allocations;
    uint32_t allocated_pages;
    uint32_t initialized;
    uint32_t address_space_ownership_verified;
} gpu_memory_stats_t;

int gpu_memory_init(gfx_backend_t backend);
int gpu_memory_alloc(gfx_backend_t backend, uint32_t page_count,
                     gpu_memory_block_t *out);
int gpu_memory_get_page(const gpu_memory_block_t *block, uint32_t page_index,
                        uint64_t *physical_address,
                        uint32_t *cpu_virtual_address);
/* Currently fails closed: no address-space ownership contract exists yet. */
int gpu_memory_get_mapping_candidate(const gpu_memory_block_t *block,
                                     uint32_t page_index,
                                     uint32_t *entry_index,
                                     uint64_t *pte_value);
/* Pins keep backing pages allocated until the matching unpin. */
int gpu_memory_pin(const gpu_memory_block_t *block);
int gpu_memory_unpin(const gpu_memory_block_t *block);
int gpu_memory_free(gpu_memory_block_t *block);
int gpu_memory_get_stats(gfx_backend_t backend, gpu_memory_stats_t *out);
/* Allocates, validates and frees one page through the generic provider API. */
int gpu_memory_selftest(gfx_backend_t backend);

#endif
