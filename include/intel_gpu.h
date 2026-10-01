#ifndef LIONOS_INTEL_GPU_H
#define LIONOS_INTEL_GPU_H

#include <stdint.h>

typedef struct {
    uint32_t found;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t revision;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint64_t bar0;
    uint32_t supported_platform;
    uint32_t firmware_framebuffer;
} intel_gpu_info_t;

/* Probe the PCI graphics function without replacing the firmware display. */
int intel_gpu_init(uint32_t firmware_framebuffer_active);
int intel_gpu_available(void);
int intel_gpu_is_alder_lake(void);
int intel_gpu_get_info(intel_gpu_info_t *out);

#endif
