#include <stdint.h>
#include "io.h"
#include "console.h"
#include "debug.h"
#include "intel_gpu.h"
#include "paging.h"
#include "memory.h"

/*
 * Alder Lake Intel display bootstrap for LionOS.
 *
 * This stage discovers the display PCI function, sizes BAR0 using the
 * standard PCI BAR sizing procedure, restores PCI configuration state,
 * and maps the verified aperture as uncached supervisor MMIO.
 *
 * No graphics-engine commands, GPU resets, display-mode changes, or
 * undocumented register writes are issued here. Firmware scanout remains
 * active. Hardware BLT is a later milestone after GGTT/stolen-memory
 * ownership and generation-specific command requirements are implemented.
 */

#define PCI_ADDR             0xCF8u
#define PCI_DATA             0xCFCu
#define PCI_COMMAND          0x04u
#define PCI_CLASS            0x08u
#define PCI_BAR0              0x10u
#define PCI_GGC               0x50u
#define PCI_BDSM              0xB4u
#define PCI_BGSM              0xB8u
#define PCI_TOLUD             0xBCu
#define PCI_COMMAND_MEMORY   0x0002u

#define INTEL_VENDOR         0x8086u
#define MMIO_VIRTUAL_BASE    0xF8000000u
#define MMIO_MAX_SIZE        0x04000000u
#define PAGE_SIZE            4096u
#define MMIO_PTE_FLAGS       0x01Au /* PWT | PCD | writable; present is added by paging. */

static intel_gpu_info_t gpu;
static uint32_t gpu_ready;
static uint32_t mmio_ready;

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

static void pci_write32(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t reg, uint32_t value)
{
    outl(PCI_ADDR, pci_key(bus, slot, func, reg));
    outl(PCI_DATA, value);
}

static uint16_t pci_read_command(uint8_t bus, uint8_t slot, uint8_t func)
{
    outl(PCI_ADDR, pci_key(bus, slot, func, PCI_COMMAND));
    return inw(PCI_DATA);
}

static void pci_write_command(uint8_t bus, uint8_t slot, uint8_t func,
                              uint16_t command)
{
    outl(PCI_ADDR, pci_key(bus, slot, func, PCI_COMMAND));
    outw(PCI_DATA, command);
}

/*
 * Read the Intel host-bridge graphics memory boundary registers. These are
 * diagnostic snapshots only: no values are decoded into allocations and no
 * memory is reserved, remapped, or modified by this probe.
 */
static void read_host_bridge_memory_config(void)
{
    uint32_t id = pci_read32(0u, 0u, 0u, 0x00u);
    uint32_t class_rev = pci_read32(0u, 0u, 0u, PCI_CLASS);
    uint16_t vendor = (uint16_t)(id & 0xFFFFu);
    uint8_t base_class = (uint8_t)(class_rev >> 24);
    uint8_t sub_class = (uint8_t)(class_rev >> 16);

    if (vendor != INTEL_VENDOR || base_class != 0x06u || sub_class != 0x00u)
        return;

    gpu.host_bridge_found = 1u;
    gpu.host_ggc = pci_read32(0u, 0u, 0u, PCI_GGC);
    gpu.host_bdsm = pci_read32(0u, 0u, 0u, PCI_BDSM);
    gpu.host_bgsm = pci_read32(0u, 0u, 0u, PCI_BGSM);
    gpu.host_tolud = pci_read32(0u, 0u, 0u, PCI_TOLUD);

    /*
     * On the verified Alder Lake display generation, conservatively exclude
     * the complete BGSM..TOLUD interval from normal RAM allocation. The
     * firmware memory map remains authoritative for all other reservations.
     */
    if (reserve_alder_lake_graphics) {
        uint64_t bgsm = (uint64_t)(gpu.host_bgsm & 0xFFF00000u);
        uint64_t bdsm = (uint64_t)(gpu.host_bdsm & 0xFFF00000u);
        uint64_t tolud = (uint64_t)(gpu.host_tolud & 0xFFF00000u);
        if (bgsm && bgsm < bdsm && bdsm < tolud && tolud <= 0x100000000ULL) {
            memory_reserve_physical_range(bgsm, tolud);
            debug_write("LIONOS:IGPU-STOLEN-RANGE-RESERVED\n");
        }
    }
}

static int intel_alder_lake_id(uint16_t id)
{
    switch (id) {
        case 0x4680u: case 0x4682u: case 0x4688u: case 0x468Au:
        case 0x468Bu: case 0x4690u: case 0x4692u: case 0x4693u:
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

/*
 * Determine BAR0's true resource size. PCI decoding is temporarily disabled
 * only for this function and the original command/BAR values are restored
 * before returning. This routine is called once during early boot.
 */
static int pci_size_bar0(uint8_t bus, uint8_t slot, uint8_t func,
                         uint64_t *base_out, uint64_t *size_out)
{
    uint32_t original_lo = pci_read32(bus, slot, func, PCI_BAR0);
    uint32_t original_hi = pci_read32(bus, slot, func, PCI_BAR0 + 4u);
    uint32_t type = (original_lo >> 1) & 3u;
    uint16_t command;
    uint32_t mask_lo, mask_hi = 0u;
    uint64_t base, mask, size;

    if ((original_lo & 1u) || type != 2u)
        return -1; /* BAR0 must be a 64-bit memory BAR. */

    base = ((uint64_t)original_hi << 32) |
           (uint64_t)(original_lo & 0xFFFFFFF0u);
    if (!base)
        return -1;

    command = pci_read_command(bus, slot, func);
    pci_write_command(bus, slot, func,
                      (uint16_t)(command & (uint16_t)~PCI_COMMAND_MEMORY));

    pci_write32(bus, slot, func, PCI_BAR0, 0xFFFFFFFFu);
    pci_write32(bus, slot, func, PCI_BAR0 + 4u, 0xFFFFFFFFu);
    mask_lo = pci_read32(bus, slot, func, PCI_BAR0);
    mask_hi = pci_read32(bus, slot, func, PCI_BAR0 + 4u);

    /* Restore the BAR pair before restoring PCI command decoding. */
    pci_write32(bus, slot, func, PCI_BAR0 + 4u, original_hi);
    pci_write32(bus, slot, func, PCI_BAR0, original_lo);
    pci_write_command(bus, slot, func, command);

    mask = ((uint64_t)mask_hi << 32) |
           (uint64_t)(mask_lo & 0xFFFFFFF0u);
    if (!mask)
        return -1;

    size = (~mask) + 1u;
    if (size < PAGE_SIZE || size > MMIO_MAX_SIZE)
        return -1;
    if ((size & (size - 1u)) != 0u)
        return -1;
    if ((base & (size - 1u)) != 0u)
        return -1;
    if (base > 0x000FFFFFFFFFFFFFULL ||
        size - 1u > 0x000FFFFFFFFFFFFFULL - base)
        return -1;

    *base_out = base;
    *size_out = size;
    return 0;
}

static int map_gpu_mmio(uint64_t physical, uint64_t size)
{
    if (!physical || !size || size > MMIO_MAX_SIZE)
        return -1;
    if ((physical & (PAGE_SIZE - 1u)) ||
        (size & (PAGE_SIZE - 1u)))
        return -1;

    for (uint32_t offset = 0u; (uint64_t)offset < size; offset += PAGE_SIZE) {
        if (paging_map_kernel_page(MMIO_VIRTUAL_BASE + offset,
                                   physical + offset,
                                   MMIO_PTE_FLAGS) != 0)
            return -1;
    }

    return 0;
}

int intel_gpu_init(uint32_t firmware_framebuffer_active)
{
    gpu_ready = 0u;
    mmio_ready = 0u;
    gpu = (intel_gpu_info_t){0};

    for (uint32_t bus = 0u; bus < 256u; ++bus) {
        for (uint32_t slot = 0u; slot < 32u; ++slot) {
            uint32_t id0 = pci_read32((uint8_t)bus, (uint8_t)slot, 0u, 0u);
            if ((id0 & 0xFFFFu) == 0xFFFFu)
                continue;

            uint32_t hdr = pci_read32((uint8_t)bus, (uint8_t)slot, 0u, 0x0Cu);
            uint32_t funcs = (hdr & 0x00800000u) ? 8u : 1u;

            for (uint32_t func = 0u; func < funcs; ++func) {
                uint32_t id = pci_read32((uint8_t)bus, (uint8_t)slot,
                                         (uint8_t)func, 0u);
                uint16_t vendor = (uint16_t)(id & 0xFFFFu);
                uint16_t device = (uint16_t)(id >> 16);
                uint32_t class_rev;
                uint8_t base_class, sub_class;
                uint64_t bar0, bar0_size;

                if (vendor != INTEL_VENDOR || func != 0u)
                    continue;

                class_rev = pci_read32((uint8_t)bus, (uint8_t)slot,
                                       (uint8_t)func, PCI_CLASS);
                base_class = (uint8_t)(class_rev >> 24);
                sub_class = (uint8_t)(class_rev >> 16);
                if (base_class != 0x03u || sub_class != 0x00u)
                    continue;

                if (pci_size_bar0((uint8_t)bus, (uint8_t)slot,
                                  (uint8_t)func, &bar0, &bar0_size) != 0) {
                    console_write("[ -- ] Intel GPU BAR0 type/size validation failed\n");
                    debug_write("LIONOS:INTEL-BAR0-INVALID\n");
                    return -1;
                }

                gpu.found = 1u;
                gpu.vendor_id = vendor;
                gpu.device_id = device;
                gpu.revision = (uint8_t)(class_rev & 0xFFu);
                gpu.bus = (uint8_t)bus;
                gpu.slot = (uint8_t)slot;
                gpu.function = (uint8_t)func;
                gpu.bar0 = bar0;
                gpu.bar0_size = bar0_size;
                gpu.mmio_virtual = MMIO_VIRTUAL_BASE;
                gpu.supported_platform = intel_known_display_id(device) ? 1u : 0u;
                gpu.firmware_framebuffer = firmware_framebuffer_active ? 1u : 0u;
                read_host_bridge_memory_config(intel_alder_lake_id(device) ? 1u : 0u);
                gpu_ready = 1u;

                console_write("[ OK ] Intel graphics PCI function ");
                console_write_hex(device);
                console_write(" BAR0=0x");
                console_write_hex((uint32_t)(bar0 >> 32));
                console_write_hex((uint32_t)bar0);
                console_write(" size=");
                console_write_dec((uint32_t)(bar0_size / (1024u * 1024u)));
                console_write(" MiB\n");

                if (gpu.host_bridge_found) {
                    console_write("[ OK ] Intel host bridge memory map (read-only)\n");
                    console_write("       GGC=0x"); console_write_hex(gpu.host_ggc);
                    console_write(" BDSM=0x"); console_write_hex(gpu.host_bdsm);
                    console_write(" BGSM=0x"); console_write_hex(gpu.host_bgsm);
                    console_write(" TOLUD=0x"); console_write_hex(gpu.host_tolud);
                    console_write("\n");
                    debug_write("LIONOS:INTEL-HOST-MEMORY-READ\n");
                } else {
                    console_write("[ -- ] Intel host bridge memory registers unavailable\n");
                }

                if (!gpu.supported_platform) {
                    console_write("[ -- ] Intel display platform not in supported ID table\n");
                    debug_write("LIONOS:INTEL-DISPLAY-GENERIC\n");
                    return 0;
                }

                if (map_gpu_mmio(bar0, bar0_size) == 0) {
                    gpu.mmio_mapped = 1u;
                    mmio_ready = 1u;
                    console_write("[ OK ] Intel GPU MMIO aperture mapped (uncached)\n");
                    debug_write("LIONOS:INTEL-MMIO-MAPPED\n");

                    /*
                     * Gen8+ GTTMMADR is 16 MiB; its GGTT PTE aperture
                     * begins at +8 MiB. GGMS encodes 2, 4, or 8 MiB
                     * when its field is 1, 2, or 3 respectively.
                     */
                    if (intel_alder_lake_id(device) &&
                        gpu.host_bridge_found && bar0_size == 0x01000000ULL) {
                        uint32_t ggms = (gpu.host_ggc >> 6) & 0x3u;
                        if (ggms != 0u) {
                            uint32_t ggtt_size = (1u << ggms) << 20;
                            uint32_t ggtt_offset = 0x00800000u;
                            if ((uint64_t)ggtt_offset + ggtt_size <= bar0_size) {
                                gpu.ggtt_offset = ggtt_offset;
                                gpu.ggtt_size = ggtt_size;
                                gpu.ggtt_ready = 1u;
                                console_write("[ OK ] Read-only GGTT aperture identified: ");
                                console_write_dec(ggtt_size / (1024u * 1024u));
                                console_write(" MiB at BAR0+0x00800000\n");
                                debug_write("LIONOS:INTEL-GGTT-READONLY-READY\n");
                            }
                        }
                    }
                } else {
                    console_write("[ -- ] Intel GPU MMIO mapping failed; firmware display retained\n");
                    debug_write("LIONOS:INTEL-MMIO-MAP-FAIL\n");
                }

                if (intel_alder_lake_id(device)) {
                    console_write("[ OK ] Alder Lake Intel graphics platform identified\n");
                    debug_write("LIONOS:INTEL-UHD-ADL\n");
                }
                if (gpu.firmware_framebuffer) {
                    console_write("[ OK ] Firmware framebuffer retained\n");
                    debug_write("LIONOS:INTEL-FB-HANDOFF\n");
                }
                return mmio_ready ? 0 : -1;
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

int intel_gpu_mmio_ready(void)
{
    return mmio_ready ? 1 : 0;
}

int intel_gpu_get_info(intel_gpu_info_t *out)
{
    if (!out)
        return -1;
    *out = gpu;
    return gpu_ready ? 0 : -1;
}

int intel_gpu_mmio_read32(uint32_t offset, uint32_t *value)
{
    if (!value || !mmio_ready || (offset & 3u) ||
        (uint64_t)offset + sizeof(uint32_t) > gpu.bar0_size)
        return -1;

    *value = *(volatile const uint32_t *)(uintptr_t)(MMIO_VIRTUAL_BASE + offset);
    return 0;
}

int intel_gpu_ggtt_read_entry(uint32_t index, uint64_t *value)
{
    uint32_t low_offset, high_offset;
    uint32_t high_before, high_after, low;
    uint32_t attempts;

    if (!value || !gpu.ggtt_ready || !mmio_ready)
        return -1;
    if (index >= gpu.ggtt_size / 4096u)
        return -1;

    low_offset = gpu.ggtt_offset + index * 8u;
    high_offset = low_offset + 4u;

    /* Bounded high-low-high sampling avoids returning a torn 64-bit PTE. */
    for (attempts = 0u; attempts < 3u; ++attempts) {
        if (intel_gpu_mmio_read32(high_offset, &high_before) != 0 ||
            intel_gpu_mmio_read32(low_offset, &low) != 0 ||
            intel_gpu_mmio_read32(high_offset, &high_after) != 0)
            return -1;

        if (high_before == high_after) {
            *value = ((uint64_t)high_after << 32) | low;
            return 0;
        }
    }

    return -2; /* Entry changed during every bounded sample. */
}
