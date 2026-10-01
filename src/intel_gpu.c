#include <stdint.h>
#include "io.h"
#include "console.h"
#include "debug.h"
#include "intel_gpu.h"

/*
 * LionOS Intel graphics bootstrap
 *
 * The Linux i915 driver uses a PCI probe, device-ID/platform matching,
 * MMIO-resource validation, and then a large generation-specific display
 * stack. LionOS is intentionally smaller: this layer performs the safe
 * PCI discovery/platform identification part and preserves the firmware
 * framebuffer selected by GRUB/UEFI.
 *
 * It does not copy i915 code and it does not attempt native Intel modesetting
 * yet. Keeping the firmware framebuffer active makes the boot path useful
 * while a native display engine is developed separately.
 */

#define PCI_ADDR       0xCF8u
#define PCI_DATA       0xCFCu
#define PCI_COMMAND    0x04u
#define PCI_REVISION   0x08u
#define PCI_CLASS      0x08u
#define PCI_BAR0       0x10u

#define INTEL_VENDOR   0x8086u

static intel_gpu_info_t gpu;
static uint32_t gpu_ready;

static uint32_t pci_key(uint8_t bus, uint8_t slot, uint8_t func, uint8_t reg)
{
    return 0x80000000u |
           ((uint32_t)bus << 16) |
           ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) |
           (reg & 0xFCu);
}

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t reg)
{
    outl(PCI_ADDR, pci_key(bus, slot, func, reg));
    return inl(PCI_DATA);
}

static int intel_alder_lake_id(uint16_t id)
{
    switch (id) {
        /* Alder Lake-S */
        case 0x4680u: case 0x4682u: case 0x4688u: case 0x468Au:
        case 0x468Bu: case 0x4690u: case 0x4692u: case 0x4693u:
        /* Alder Lake-P / H class family IDs used by the i915 driver */
        case 0x46A0u: case 0x46A1u: case 0x46A2u: case 0x46A3u:
        case 0x46A6u: case 0x46A8u: case 0x46AAu: case 0x462Au:
        case 0x4626u: case 0x4628u: case 0x46B0u: case 0x46B1u:
        case 0x46B2u: case 0x46B3u: case 0x46C0u: case 0x46C1u:
        case 0x46C2u: case 0x46C3u:
            return 1;
        default:
            return 0;
    }
}

static int intel_known_display_id(uint16_t id)
{
    if (intel_alder_lake_id(id))
        return 1;

    switch (id) {
        /* Raptor Lake display IDs are kept here because their display
         * path follows the same firmware-framebuffer-first bootstrap. */
        case 0xA780u: case 0xA781u: case 0xA782u: case 0xA783u:
        case 0xA788u: case 0xA789u: case 0xA78Au: case 0xA78Bu:
        case 0xA720u: case 0xA721u: case 0xA7A0u: case 0xA7A1u:
        case 0xA7A8u: case 0xA7A9u: case 0xA7AAu: case 0xA7ABu:
        case 0xA7ACu: case 0xA7ADu:
            return 1;
        default:
            return 0;
    }
}

static uint64_t pci_bar0_address(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint32_t lo = pci_read32(bus, slot, func, PCI_BAR0);
    if (!lo || (lo & 1u))
        return 0;

    uint64_t addr = (uint64_t)(lo & 0xFFFFFFF0u);
    if (((lo >> 1) & 3u) == 2u)
        addr |= (uint64_t)pci_read32(bus, slot, func, PCI_BAR0 + 4u) << 32;

    return addr;
}

int intel_gpu_init(uint32_t firmware_framebuffer_active)
{
    gpu_ready = 0u;
    gpu = (intel_gpu_info_t){0};

    for (uint32_t bus = 0u; bus < 256u; ++bus) {
        for (uint32_t slot = 0u; slot < 32u; ++slot) {
            uint32_t id0 = pci_read32((uint8_t)bus, (uint8_t)slot, 0u, 0u);
            if ((id0 & 0xFFFFu) == 0xFFFFu)
                continue;

            uint32_t hdr = pci_read32((uint8_t)bus, (uint8_t)slot, 0u, 0x0Cu);
            uint32_t funcs = (hdr & 0x00800000u) ? 8u : 1u;

            for (uint32_t func = 0u; func < funcs; ++func) {
                uint32_t id = pci_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0u);
                uint16_t vendor = (uint16_t)(id & 0xFFFFu);
                uint16_t device = (uint16_t)(id >> 16);

                if (vendor != INTEL_VENDOR)
                    continue;

                /*
                 * Follow the important part of the i915 PCI policy:
                 * bind the display function, not an auxiliary function.
                 */
                if (func != 0u)
                    continue;

                uint32_t class_rev =
                    pci_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, PCI_CLASS);
                uint8_t base_class = (uint8_t)(class_rev >> 24);
                uint8_t sub_class = (uint8_t)(class_rev >> 16);

                if (base_class != 0x03u || sub_class != 0x00u)
                    continue;

                uint64_t bar0 = pci_bar0_address((uint8_t)bus, (uint8_t)slot, (uint8_t)func);
                if (!bar0)
                    continue;

                gpu.found = 1u;
                gpu.vendor_id = vendor;
                gpu.device_id = device;
                gpu.revision = (uint8_t)(class_rev & 0xFFu);
                gpu.bus = (uint8_t)bus;
                gpu.slot = (uint8_t)slot;
                gpu.function = (uint8_t)func;
                gpu.bar0 = bar0;
                gpu.supported_platform = intel_known_display_id(device) ? 1u : 0u;
                gpu.firmware_framebuffer = firmware_framebuffer_active ? 1u : 0u;
                gpu_ready = 1u;

                console_write("[ OK ] Intel graphics controller ");
                console_write_hex(device);
                console_write(" detected at PCI ");
                console_write_dec(bus);
                console_putc(':');
                console_write_dec(slot);
                console_putc('.');
                console_write_dec(func);
                console_write(" BAR0=0x");
                console_write_hex((uint32_t)bar0);
                console_write("\n");

                if (intel_alder_lake_id(device)) {
                    console_write("[ OK ] Alder Lake Intel display platform identified\n");
                    debug_write("LIONOS:INTEL-UHD-ADL\n");
                } else if (gpu.supported_platform) {
                    console_write("[ OK ] Intel modern display platform identified\n");
                    debug_write("LIONOS:INTEL-DISPLAY-PLATFORM\n");
                } else {
                    console_write("[ -- ] Intel display ID is not in the LionOS platform table\n");
                    debug_write("LIONOS:INTEL-DISPLAY-GENERIC\n");
                }

                if (gpu.firmware_framebuffer) {
                    console_write("[ OK ] Firmware framebuffer retained for Intel display\n");
                    debug_write("LIONOS:INTEL-FB-HANDOFF\n");
                } else {
                    console_write("[ -- ] No firmware framebuffer; text/display fallback remains active\n");
                }

                return 0;
            }
        }
    }

    debug_write("LIONOS:INTEL-GPU-NOT-FOUND\n");
    return -1;
}

int intel_gpu_available(void)
{
    return gpu_ready ? 1 : 0;
}

int intel_gpu_is_alder_lake(void)
{
    return gpu_ready && intel_alder_lake_id(gpu.device_id);
}

int intel_gpu_get_info(intel_gpu_info_t *out)
{
    if (!out)
        return -1;
    *out = gpu;
    return gpu_ready ? 0 : -1;
}
