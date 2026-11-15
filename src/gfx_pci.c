#include <stdint.h>
#include "gfx_pci.h"
#include "pci.h"

int gfx_pci_classify(uint16_t vendor_id, uint8_t class_code,
                     gfx_backend_t *backend)
{
    if (!backend)
        return -1;

    *backend = GFX_BACKEND_NONE;
    if (class_code != 0x03u)
        return 0;

    switch (vendor_id) {
    case 0x8086u:
        *backend = GFX_BACKEND_INTEL_GPU;
        break;
    case 0x1002u:
        *backend = GFX_BACKEND_AMD_GPU;
        break;
    case 0x10DEu:
        *backend = GFX_BACKEND_NVIDIA_GPU;
        break;
    default:
        break;
    }

    return 1;
}

int gfx_pci_discover(gfx_pci_discovery_report_t *out)
{
    gfx_pci_discovery_report_t report = {0};
    uint32_t recorded;
    uint32_t discovered;

    if (!out || !pci_is_initialized() ||
        gfx_backend_registry_init() != 0)
        return -1;

    recorded = pci_recorded_device_count();
    discovered = pci_device_count();
    if (recorded > discovered)
        return -1;

    report.pci_discovered_devices = discovered;
    report.pci_recorded_devices = recorded;
    report.pci_recording_truncated = discovered > recorded ? 1u : 0u;

    for (uint32_t i = 0u; i < recorded; ++i) {
        pci_device_info_t device;
        gfx_pci_adapter_t adapter;
        gfx_backend_t backend;
        int is_display;

        if (pci_get_device(i, &device) != 0)
            return -1;

        is_display = gfx_pci_classify(device.vendor_id,
                                      device.class_code, &backend);
        if (is_display < 0)
            return -1;
        if (is_display == 0)
            continue;

        ++report.display_adapter_count;
        adapter.backend = backend;
        adapter.vendor_id = device.vendor_id;
        adapter.device_id = device.device_id;
        adapter.bus = device.bus;
        adapter.device = device.device;
        adapter.function = device.function;
        adapter.revision = device.revision;
        adapter.class_code = device.class_code;
        adapter.subclass = device.subclass;
        adapter.programming_interface = device.programming_interface;

        if (backend == GFX_BACKEND_NONE) {
            ++report.unsupported_adapter_count;
        } else {
            gfx_backend_state_t state = gfx_backend_get_state(backend);
            ++report.supported_adapter_count;

            /*
             * PCI class/vendor matching proves detection only. Preserve a
             * stronger state established by a vendor driver, if one exists.
             */
            if (state == GFX_BACKEND_STATE_UNAVAILABLE &&
                gfx_backend_report_state(backend,
                                         GFX_BACKEND_STATE_DETECTED) != 0)
                return -1;
        }

        if (report.stored_adapter_count < GFX_PCI_MAX_ADAPTERS) {
            report.adapters[report.stored_adapter_count] = adapter;
            ++report.stored_adapter_count;
        } else {
            report.adapter_list_truncated = 1u;
        }
    }

    *out = report;
    return 0;
}

int gfx_pci_discovery_selftest(void)
{
    gfx_backend_t backend = GFX_BACKEND_NONE;

    if (gfx_pci_classify(0x8086u, 0x03u, &backend) != 1 ||
        backend != GFX_BACKEND_INTEL_GPU)
        return -1;

    if (gfx_pci_classify(0x1002u, 0x03u, &backend) != 1 ||
        backend != GFX_BACKEND_AMD_GPU)
        return -1;

    if (gfx_pci_classify(0x10DEu, 0x03u, &backend) != 1 ||
        backend != GFX_BACKEND_NVIDIA_GPU)
        return -1;

    if (gfx_pci_classify(0x1234u, 0x03u, &backend) != 1 ||
        backend != GFX_BACKEND_NONE)
        return -1;

    if (gfx_pci_classify(0x8086u, 0x02u, &backend) != 0 ||
        backend != GFX_BACKEND_NONE)
        return -1;

    if (gfx_pci_classify(0x8086u, 0x03u, 0) != -1)
        return -1;

    return 0;
}
