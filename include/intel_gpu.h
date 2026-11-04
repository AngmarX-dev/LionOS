#ifndef LIONOS_INTEL_GPU_H
#define LIONOS_INTEL_GPU_H

#include <stdint.h>

typedef struct {
    uint32_t head;
    uint32_t tail;
    uint32_t start;
    uint32_t control;
} intel_gpu_ring_snapshot_t;

typedef struct {
    uint32_t valid;
    uint32_t graphics_gmd_id;
    uint32_t graphics_gmd_id_valid;
    intel_gpu_ring_snapshot_t render;
    intel_gpu_ring_snapshot_t blitter;
} intel_gpu_engine_snapshot_t;

typedef enum {
    INTEL_GT_DIAG_NOT_RUN = 0,
    INTEL_GT_DIAG_READY,
    INTEL_GT_DIAG_GT_FW_MMIO,
    INTEL_GT_DIAG_GT_FW_ACK,
    INTEL_GT_DIAG_GT_ID,
    INTEL_GT_DIAG_RENDER_FW_MMIO,
    INTEL_GT_DIAG_RENDER_FW_ACK,
    INTEL_GT_DIAG_RCS_READ,
    INTEL_GT_DIAG_BCS_READ,
    INTEL_GT_DIAG_RELEASE
} intel_gt_diag_code_t;

typedef struct {
    uint32_t code;
    uint32_t gt_ack_initial;
    uint32_t gt_ack_final;
    uint32_t render_ack_initial;
    uint32_t render_ack_final;
    uint32_t gt_fallback_used;
    uint32_t render_fallback_used;
} intel_gt_diag_t;

typedef struct {
    uint32_t found;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t revision;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint64_t bar0;
    uint64_t bar0_size;
    uint32_t mmio_virtual;
    uint32_t mmio_mapped;
    uint32_t supported_platform;
    uint32_t firmware_framebuffer;
    uint32_t host_bridge_found;
    uint32_t host_ggc;
    uint32_t host_bdsm;
    uint32_t host_bgsm;
    uint32_t host_tolud;
    uint32_t ggtt_offset;
    uint32_t ggtt_size;
    uint32_t ggtt_ready;
} intel_gpu_info_t;

/* PCI discovery and conservative, read-only MMIO aperture setup. */
int intel_gpu_init(uint32_t firmware_framebuffer_active);
int intel_gpu_available(void);
int intel_gpu_is_alder_lake(void);
int intel_gpu_mmio_ready(void);
int intel_gpu_get_info(intel_gpu_info_t *out);

/* Read-only MMIO accessor. Returns -1 for an unmapped/out-of-range access. */
int intel_gpu_mmio_read32(uint32_t offset, uint32_t *value);
/* Read one existing Gen8+ GGTT PTE without modifying the table. */
int intel_gpu_ggtt_read_entry(uint32_t index, uint64_t *value);
/* Read-only Alder Lake GT identity and ring-register snapshot. */
int intel_gpu_get_engine_snapshot(intel_gpu_engine_snapshot_t *out);
int intel_gpu_get_gt_diagnostics(intel_gt_diag_t *out);

/*
 * GPU backing-memory manager, first bring-up stage.
 * GGTT entries are scanned and reserved in software only; no GGTT PTE is
 * modified and buffers are NOT GPU-addressable until the later mapping stage.
 * All manager operations are BSP-only and must be externally serialized.
 */
#define INTEL_GPU_MEMORY_MAX_PAGES 256u

typedef struct {
    uint32_t handle;
    uint32_t page_count;
    uint32_t ggtt_first_entry;
    uint32_t bytes;
    uint32_t ggtt_mapping_active; /* Always zero in this stage. */
} intel_gpu_memory_block_t;

typedef struct {
    uint32_t ggtt_total_entries;
    uint32_t ggtt_free_entries;
    uint32_t active_allocations;
    uint32_t allocated_pages;
    uint32_t initialized;
} intel_gpu_memory_stats_t;

int intel_gpu_memory_init(void);
int intel_gpu_memory_alloc(uint32_t page_count, intel_gpu_memory_block_t *out);
int intel_gpu_memory_free(uint32_t handle);
int intel_gpu_memory_get_page(uint32_t handle, uint32_t page_index,
                              uint64_t *physical_address,
                              uint32_t *cpu_virtual_address);
int intel_gpu_memory_get_stats(intel_gpu_memory_stats_t *out);
int intel_gpu_memory_selftest(void);

#endif
