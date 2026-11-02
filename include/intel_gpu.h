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
    intel_gpu_ring_snapshot_t render;
    intel_gpu_ring_snapshot_t blitter;
} intel_gpu_engine_snapshot_t;

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

#endif
