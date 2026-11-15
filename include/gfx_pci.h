#ifndef LIONOS_GFX_PCI_H
#define LIONOS_GFX_PCI_H

#include <stdint.h>
#include "gfx.h"

#define GFX_PCI_MAX_ADAPTERS 16u

typedef struct {
    gfx_backend_t backend; /* NONE for an unrecognized vendor. */
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t revision;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t programming_interface;
} gfx_pci_adapter_t;

typedef struct {
    uint32_t pci_discovered_devices;
    uint32_t pci_recorded_devices;
    uint32_t pci_recording_truncated;
    uint32_t display_adapter_count;
    uint32_t supported_adapter_count;
    uint32_t unsupported_adapter_count;
    uint32_t stored_adapter_count;
    uint32_t adapter_list_truncated;
    gfx_pci_adapter_t adapters[GFX_PCI_MAX_ADAPTERS];
} gfx_pci_discovery_report_t;

/*
 * Classify PCI display-class functions without touching device registers.
 * Returns 1 for class 03h, 0 for other classes, and -1 for invalid output.
 * Unknown display vendors are reported as GFX_BACKEND_NONE.
 */
int gfx_pci_classify(uint16_t vendor_id, uint8_t class_code,
                     gfx_backend_t *backend);

/* Read the already-enumerated PCI table and mark known GPU vendors DETECTED. */
int gfx_pci_discover(gfx_pci_discovery_report_t *out);

/* Pure vendor/classification tests; does not access PCI configuration space. */
int gfx_pci_discovery_selftest(void);

#endif
