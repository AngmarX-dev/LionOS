#include <stdint.h>
#include "io.h"
#include "console.h"
#include "debug.h"
#include "intel_gpu.h"
#include "paging.h"
#include "memory.h"
#include "cpu.h"

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

/* Read-only Gen12 GT/engine register snapshot; no command submission. */
#define INTEL_GMD_ID_GRAPHICS       0x00000D8Cu
#define INTEL_RENDER_RING_BASE     0x00002000u
#define INTEL_BLITTER_RING_BASE    0x00022000u
#define INTEL_RING_HEAD_OFFSET     0x34u
#define INTEL_RING_TAIL_OFFSET     0x30u
#define INTEL_RING_START_OFFSET    0x38u
#define INTEL_RING_CTL_OFFSET      0x3Cu

static intel_gpu_engine_snapshot_t engine_snapshot;
static intel_gt_diag_t gt_diagnostics;
static void intel_gpu_hex32(uint32_t value);

static int intel_gpu_mmio_write32(uint32_t offset, uint32_t value)
{
    if (!mmio_ready || (offset & 3u) ||
        (uint64_t)offset + sizeof(uint32_t) > gpu.bar0_size)
        return -1;
    *(volatile uint32_t *)(uintptr_t)(MMIO_VIRTUAL_BASE + offset) = value;
    (void)*(volatile const uint32_t *)(uintptr_t)(MMIO_VIRTUAL_BASE + offset);
    return 0;
}

static int intel_gpu_read_engine_registers(uint32_t base,
                                           intel_gpu_ring_snapshot_t *ring)
{
    if (!ring || !mmio_ready)
        return -1;
    if (intel_gpu_mmio_read32(base + INTEL_RING_HEAD_OFFSET, &ring->head) != 0 ||
        intel_gpu_mmio_read32(base + INTEL_RING_TAIL_OFFSET, &ring->tail) != 0 ||
        intel_gpu_mmio_read32(base + INTEL_RING_START_OFFSET, &ring->start) != 0 ||
        intel_gpu_mmio_read32(base + INTEL_RING_CTL_OFFSET, &ring->control) != 0)
        return -1;
    return 0;
}

/*
 * Gen9+ Forcewake protocol aligned with Linux i915 intel_uncore.c.
 * Alder Lake (12th Gen) uses FORCEWAKE_GT_GEN9 / ACK_GT_GEN9,
 * and FORCEWAKE_RENDER_GEN9 / ACK_RENDER_GEN9.
 */
#define INTEL_FORCEWAKE_GT_REQ          0x0000A188u
#define INTEL_FORCEWAKE_RENDER_REQ      0x0000A278u
#define INTEL_FORCEWAKE_GT_ACK          0x000130044u
#define INTEL_FORCEWAKE_RENDER_ACK      0x00000D84u
#define INTEL_FORCEWAKE_KERNEL          (1u << 0)
#define INTEL_FORCEWAKE_KERNEL_FALLBACK (1u << 15)
#define INTEL_FORCEWAKE_MASKED_SET(bit) (((bit) << 16) | (bit))
#define INTEL_FORCEWAKE_MASKED_CLR(bit) ((bit) << 16)
#define INTEL_FORCEWAKE_ACK_POLLS       1000000u

typedef struct {
    uint32_t request_reg;
    uint32_t ack_reg;
    uint32_t ack_initial;
    uint32_t ack_final;
    uint32_t fallback_used;
    uint32_t fallback_passes;
    uint32_t preexisting_kernel_ack;
} intel_forcewake_result_t;

static int intel_gpu_wait_ack(uint32_t ack_reg, uint32_t mask,
                              uint32_t expected, uint32_t *last_value)
{
    uint32_t value = 0u;
    for (uint32_t i = 0u; i < INTEL_FORCEWAKE_ACK_POLLS; ++i) {
        if (intel_gpu_mmio_read32(ack_reg, &value) != 0 ||
            value == 0xFFFFFFFFu) {
            if (last_value) *last_value = value;
            return -2; /* MMIO access failure / all-ones response. */
        }
        if ((value & mask) == expected) {
            if (last_value) *last_value = value;
            return 0;
        }
        __asm__ volatile("pause");
    }
    if (last_value) *last_value = value;
    return -1; /* ACK timeout. */
}

/*
 * Linux i915's fallback workaround toggles bit 15 after the normal ACK
 * wait fails. The increasing delay gives the GT state machine time to
 * deliver the delayed kernel ACK. We use bounded pause loops because this
 * early boot stage does not yet have a calibrated microsecond delay.
 */
static int intel_gpu_forcewake_fallback(uint32_t request_reg,
                                        uint32_t ack_reg,
                                        uint32_t desired_kernel_ack,
                                        intel_forcewake_result_t *result)
{
    for (uint32_t pass = 1u; pass <= 10u; ++pass) {
        uint32_t ack = 0u;

        if (intel_gpu_wait_ack(ack_reg, INTEL_FORCEWAKE_KERNEL_FALLBACK,
                               0u, &ack) != 0)
            continue;

        if (intel_gpu_mmio_write32(
                request_reg,
                INTEL_FORCEWAKE_MASKED_SET(INTEL_FORCEWAKE_KERNEL_FALLBACK)) != 0)
            return -2;

        /* Bounded backoff; deliberately no uncalibrated microsecond claim. */
        for (uint32_t delay = 0u; delay < pass * 1000u; ++delay)
            __asm__ volatile("pause");

        (void)intel_gpu_wait_ack(ack_reg,
                                 INTEL_FORCEWAKE_KERNEL_FALLBACK,
                                 INTEL_FORCEWAKE_KERNEL_FALLBACK, &ack);

        if (intel_gpu_mmio_read32(ack_reg, &ack) != 0 ||
            ack == 0xFFFFFFFFu) {
            (void)intel_gpu_mmio_write32(
                request_reg,
                INTEL_FORCEWAKE_MASKED_CLR(INTEL_FORCEWAKE_KERNEL_FALLBACK));
            return -2;
        }

        if ((ack & INTEL_FORCEWAKE_KERNEL) == desired_kernel_ack) {
            result->fallback_used = 1u;
            result->fallback_passes = pass;
            (void)intel_gpu_mmio_write32(
                request_reg,
                INTEL_FORCEWAKE_MASKED_CLR(INTEL_FORCEWAKE_KERNEL_FALLBACK));
            return 0;
        }

        if (intel_gpu_mmio_write32(
                request_reg,
                INTEL_FORCEWAKE_MASKED_CLR(INTEL_FORCEWAKE_KERNEL_FALLBACK)) != 0)
            return -2;
    }
    return -1;
}

static int intel_gpu_forcewake_acquire(uint32_t request_reg,
                                       uint32_t ack_reg,
                                       intel_forcewake_result_t *result)
{
    uint32_t ack = 0u;
    int rc;

    if (!result)
        return -2;
    *result = (intel_forcewake_result_t){0};
    result->request_reg = request_reg;
    result->ack_reg = ack_reg;

    rc = intel_gpu_mmio_read32(ack_reg, &ack);
    if (rc != 0 || ack == 0xFFFFFFFFu)
        return -2;
    result->ack_initial = ack;
    if ((ack & INTEL_FORCEWAKE_KERNEL) != 0u) {
        /* Do not clear a forcewake reference that predates this probe. */
        result->preexisting_kernel_ack = 1u;
        result->ack_final = ack;
        return 0;
    }

    /* Linux i915 first waits for the previous kernel request to clear. */
    rc = intel_gpu_wait_ack(ack_reg, INTEL_FORCEWAKE_KERNEL, 0u, &ack);
    if (rc != 0) {
        int frc = intel_gpu_forcewake_fallback(request_reg, ack_reg, 0u, result);
        if (frc != 0)
            return frc;
    }

    if (intel_gpu_mmio_write32(
            request_reg,
            INTEL_FORCEWAKE_MASKED_SET(INTEL_FORCEWAKE_KERNEL)) != 0)
        return -2;

    rc = intel_gpu_wait_ack(ack_reg, INTEL_FORCEWAKE_KERNEL,
                            INTEL_FORCEWAKE_KERNEL, &ack);
    if (rc != 0) {
        int frc = intel_gpu_forcewake_fallback(
            request_reg, ack_reg, INTEL_FORCEWAKE_KERNEL, result);
        if (frc != 0) {
            (void)intel_gpu_mmio_write32(
                request_reg,
                INTEL_FORCEWAKE_MASKED_CLR(INTEL_FORCEWAKE_KERNEL));
            return frc;
        }
        if (intel_gpu_mmio_read32(ack_reg, &ack) != 0 ||
            (ack & INTEL_FORCEWAKE_KERNEL) == 0u) {
            (void)intel_gpu_mmio_write32(
                request_reg,
                INTEL_FORCEWAKE_MASKED_CLR(INTEL_FORCEWAKE_KERNEL));
            return -1;
        }
    }

    result->ack_final = ack;
    return 0;
}

static int intel_gpu_forcewake_release(uint32_t request_reg,
                                       uint32_t ack_reg,
                                       intel_forcewake_result_t *result)
{
    uint32_t ack = 0u;
    if (result && result->preexisting_kernel_ack)
        return 0;
    if (intel_gpu_mmio_write32(
            request_reg,
            INTEL_FORCEWAKE_MASKED_CLR(INTEL_FORCEWAKE_KERNEL)) != 0)
        return -2;

    /* Match i915: wait for the kernel ACK to clear after dropping request. */
    if (intel_gpu_wait_ack(ack_reg, INTEL_FORCEWAKE_KERNEL, 0u, &ack) != 0)
        return -1;
    if (result)
        result->ack_final = ack;
    return 0;
}

static void intel_gpu_probe_engine_registers(void)
{
    intel_forcewake_result_t gt_fw = {0}, render_fw = {0};
    int gt_acquired = 0, render_acquired = 0, rc;

    if (!intel_gpu_is_alder_lake() || !mmio_ready)
        return;

    engine_snapshot = (intel_gpu_engine_snapshot_t){0};
    gt_diagnostics = (intel_gt_diag_t){0};

    rc = intel_gpu_forcewake_acquire(INTEL_FORCEWAKE_GT_REQ,
                                     INTEL_FORCEWAKE_GT_ACK, &gt_fw);
    gt_diagnostics.gt_ack_initial = gt_fw.ack_initial;
    gt_diagnostics.gt_ack_final = gt_fw.ack_final;
    gt_diagnostics.gt_fallback_used = gt_fw.fallback_used;
    if (rc != 0) {
        gt_diagnostics.code = (rc == -2) ? INTEL_GT_DIAG_GT_FW_MMIO : INTEL_GT_DIAG_GT_FW_ACK;
        debug_write(rc == -2 ? "LIONOS:INTEL-GT-FW-MMIO-FAIL\n" :
                               "LIONOS:INTEL-GT-FW-ACK-FAIL\n");
        console_write("[ -- ] GT Forcewake failed\n");
        return;
    }
    gt_acquired = gt_fw.preexisting_kernel_ack ? 0 : 1;

    /*
     * Alder Lake (including PCI ID 8086:468B) does not expose GMD_ID as
     * a supported capability. Linux i915 gates this register through
     * HAS_GMD_ID; it must not be used as a GT-presence test on Alder Lake.
     * Keep the optional field invalid and continue with engine diagnostics.
     */
    engine_snapshot.graphics_gmd_id = 0u;
    engine_snapshot.graphics_gmd_id_valid = 0u;
    debug_write("LIONOS:INTEL-GMD-ID-NOT-APPLICABLE-ADL\n");

    rc = intel_gpu_forcewake_acquire(INTEL_FORCEWAKE_RENDER_REQ,
                                     INTEL_FORCEWAKE_RENDER_ACK, &render_fw);
    gt_diagnostics.render_ack_initial = render_fw.ack_initial;
    gt_diagnostics.render_ack_final = render_fw.ack_final;
    gt_diagnostics.render_fallback_used = render_fw.fallback_used;
    if (rc != 0) {
        gt_diagnostics.code = (rc == -2) ? INTEL_GT_DIAG_RENDER_FW_MMIO : INTEL_GT_DIAG_RENDER_FW_ACK;
        debug_write(rc == -2 ? "LIONOS:INTEL-RENDER-FW-MMIO-FAIL\n" :
                               "LIONOS:INTEL-RENDER-FW-ACK-FAIL\n");
        goto cleanup;
    }
    render_acquired = render_fw.preexisting_kernel_ack ? 0 : 1;

    if (intel_gpu_read_engine_registers(INTEL_RENDER_RING_BASE,
                                        &engine_snapshot.render) != 0) {
        gt_diagnostics.code = INTEL_GT_DIAG_RCS_READ;
        debug_write("LIONOS:INTEL-RCS-READ-FAIL\n");
        goto cleanup;
    }
    if (intel_gpu_read_engine_registers(INTEL_BLITTER_RING_BASE,
                                        &engine_snapshot.blitter) != 0) {
        gt_diagnostics.code = INTEL_GT_DIAG_BCS_READ;
        debug_write("LIONOS:INTEL-BCS-READ-FAIL\n");
        goto cleanup;
    }

    engine_snapshot.valid = 1u;
    gt_diagnostics.code = INTEL_GT_DIAG_READY;
    console_write("[ OK ] Gen12 GT/RCS/BCS registers sampled\n");
    debug_write("LIONOS:INTEL-GT-I915-FORCEWAKE-PROBE-OK\n");

cleanup:
    if (render_acquired &&
        intel_gpu_forcewake_release(INTEL_FORCEWAKE_RENDER_REQ,
                                    INTEL_FORCEWAKE_RENDER_ACK,
                                    &render_fw) != 0) {
        gt_diagnostics.code = INTEL_GT_DIAG_RELEASE;
        debug_write("LIONOS:INTEL-RENDER-FW-RELEASE-FAIL\n");
    }
    if (gt_acquired &&
        intel_gpu_forcewake_release(INTEL_FORCEWAKE_GT_REQ,
                                    INTEL_FORCEWAKE_GT_ACK,
                                    &gt_fw) != 0) {
        gt_diagnostics.code = INTEL_GT_DIAG_RELEASE;
        debug_write("LIONOS:INTEL-GT-FW-RELEASE-FAIL\n");
    }
}
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
 * Read Intel host-bridge graphics memory boundaries. On a matched Alder Lake
 * device, reserve the validated BGSM..TOLUD interval from general PMM use.
 */
static void read_host_bridge_memory_config(uint32_t reserve_alder_lake_graphics)
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
            console_write("[ OK ] Alder Lake graphics BGSM..TOLUD memory reserved from PMM\n");
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

/*
 * PCI device IDs are grouped by Intel's upstream i915 platform tables.
 * Keep the displayed family precise: ADL-S and ADL-P are distinct
 * platform families even though both use the Alder Lake display IP.
 */
static const char *intel_gpu_platform_name_for_id(uint16_t id)
{
    switch (id) {
        case 0x4680u: case 0x4682u: case 0x4688u: case 0x468Au:
        case 0x468Bu: case 0x4690u: case 0x4692u: case 0x4693u:
            return "ALDER LAKE-S";

        case 0x46A0u: case 0x46A1u: case 0x46A2u: case 0x46A3u:
        case 0x46A6u: case 0x46A8u: case 0x46AAu: case 0x462Au:
        case 0x4626u: case 0x4628u: case 0x46B0u: case 0x46B1u:
        case 0x46B2u: case 0x46B3u: case 0x46C0u: case 0x46C1u:
        case 0x46C2u: case 0x46C3u:
            return "ALDER LAKE-P";

        case 0xA780u: case 0xA781u: case 0xA782u: case 0xA783u:
        case 0xA788u: case 0xA789u: case 0xA78Au: case 0xA78Bu:
            return "RAPTOR LAKE-S";

        default:
            return "SUPPORTED / OTHER";
    }
}

const char *intel_gpu_platform_name(void)
{
    if (!gpu_ready)
        return "NOT DETECTED";
    return intel_gpu_platform_name_for_id(gpu.device_id);
}

const char *intel_gpu_gmd_id_status(void)
{
    if (!gpu_ready)
        return "NOT DETECTED";
    switch (gpu.device_id) {
        case 0x4680u: case 0x4682u: case 0x4688u: case 0x468Au:
        case 0x468Bu: case 0x4690u: case 0x4692u: case 0x4693u:
            return "N/A (ADL-S)";
        case 0x46A0u: case 0x46A1u: case 0x46A2u: case 0x46A3u:
        case 0x46A6u: case 0x46A8u: case 0x46AAu: case 0x462Au:
        case 0x4626u: case 0x4628u: case 0x46B0u: case 0x46B1u:
        case 0x46B2u: case 0x46B3u: case 0x46C0u: case 0x46C1u:
        case 0x46C2u: case 0x46C3u:
            return "N/A (ADL-P)";
        case 0xA780u: case 0xA781u: case 0xA782u: case 0xA783u:
        case 0xA788u: case 0xA789u: case 0xA78Au: case 0xA78Bu:
            return "N/A (RPL-S)";
        default:
            return "NOT REPORTED";
    }
}

uint32_t intel_gpu_platform_engine_mask(void)
{
    if (!gpu_ready)
        return 0u;

    switch (gpu.device_id) {
        /* Upstream i915 ADL-S platform_engine_mask. */
        case 0x4680u: case 0x4682u: case 0x4688u: case 0x468Au:
        case 0x468Bu: case 0x4690u: case 0x4692u: case 0x4693u:
        /* Raptor Lake-S currently reuses the ADL-S device-info table. */
        case 0xA780u: case 0xA781u: case 0xA782u: case 0xA783u:
        case 0xA788u: case 0xA789u: case 0xA78Au: case 0xA78Bu:
            return INTEL_GPU_ENGINE_RCS0 | INTEL_GPU_ENGINE_BCS0 |
                   INTEL_GPU_ENGINE_VECS0 | INTEL_GPU_ENGINE_VCS0 |
                   INTEL_GPU_ENGINE_VCS2;

        /* Upstream i915 ADL-P platform_engine_mask. */
        case 0x46A0u: case 0x46A1u: case 0x46A2u: case 0x46A3u:
        case 0x46A6u: case 0x46A8u: case 0x46AAu: case 0x462Au:
        case 0x4626u: case 0x4628u: case 0x46B0u: case 0x46B1u:
        case 0x46B2u: case 0x46B3u: case 0x46C0u: case 0x46C1u:
        case 0x46C2u: case 0x46C3u:
            return INTEL_GPU_ENGINE_RCS0 | INTEL_GPU_ENGINE_BCS0 |
                   INTEL_GPU_ENGINE_VECS0 | INTEL_GPU_ENGINE_VCS0 |
                   INTEL_GPU_ENGINE_VCS2;

        default:
            return 0u;
    }
}

/*
 * Mirror the upstream i915 default policy for these PCI platform groups.
 * This is policy metadata only: it does not detect a loaded firmware image,
 * select a scheduler in LionOS, or claim that any submission path is active.
 *
 * ADL-S defaults to HuC authentication only (GuC submission is not selected).
 * Other supported Gen12 groups use the upstream default GuC submission policy.
 */
const char *intel_gpu_submission_policy(void)
{
    if (!gpu_ready)
        return "NOT DETECTED";

    switch (gpu.device_id) {
        case 0x4680u: case 0x4682u: case 0x4688u: case 0x468Au:
        case 0x468Bu: case 0x4690u: case 0x4692u: case 0x4693u:
            return "EXECLISTS DEFAULT / GUC SUB OFF";

        case 0x46A0u: case 0x46A1u: case 0x46A2u: case 0x46A3u:
        case 0x46A6u: case 0x46A8u: case 0x46AAu: case 0x462Au:
        case 0x4626u: case 0x4628u: case 0x46B0u: case 0x46B1u:
        case 0x46B2u: case 0x46B3u: case 0x46C0u: case 0x46C1u:
        case 0x46C2u: case 0x46C3u:
        case 0xA780u: case 0xA781u: case 0xA782u: case 0xA783u:
        case 0xA788u: case 0xA789u: case 0xA78Au: case 0xA78Bu:
            return "GUC SUBMISSION DEFAULT";

        default:
            return "POLICY UNKNOWN";
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

/*
 * Read-only GGTT aperture smoke test. This checks that the first and last
 * PTE slots are reachable through the mapped BAR without changing firmware
 * page-table contents or submitting work to the graphics engine.
 */
static void intel_gpu_hex32(uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4)
        console_putc(digits[(value >> (uint32_t)shift) & 0x0Fu]);
}

static void intel_gpu_probe_ggtt(void)
{
    uint64_t first = 0u, last = 0u;
    uint32_t last_index;

    if (!gpu.ggtt_ready || !mmio_ready || gpu.ggtt_size < 8u)
        return;

    last_index = gpu.ggtt_size / 4096u - 1u;
    if (intel_gpu_ggtt_read_entry(0u, &first) != 0 ||
        intel_gpu_ggtt_read_entry(last_index, &last) != 0) {
        console_write("[ -- ] Intel GGTT read-only aperture probe failed\n");
        debug_write("LIONOS:INTEL-GGTT-READ-PROBE-FAIL\n");
        return;
    }

    console_write("[ OK ] Intel GGTT read-only boundary probe: first=0x");
    intel_gpu_hex32((uint32_t)(first >> 32));
    intel_gpu_hex32((uint32_t)first);
    console_write(" last=0x");
    intel_gpu_hex32((uint32_t)(last >> 32));
    intel_gpu_hex32((uint32_t)last);
    console_write("\n");
    debug_write("LIONOS:INTEL-GGTT-READ-PROBE-OK\n");
}

int intel_gpu_init(uint32_t firmware_framebuffer_active)
{
    gpu_ready = 0u;
    mmio_ready = 0u;
    gpu = (intel_gpu_info_t){0};
    engine_snapshot = (intel_gpu_engine_snapshot_t){0};
    gt_diagnostics = (intel_gt_diag_t){0};

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
                if (bar0 <= UINT64_MAX - bar0_size)
                    memory_reserve_physical_range(bar0, bar0 + bar0_size);
                gpu.supported_platform = intel_known_display_id(device) ? 1u : 0u;
                gpu.firmware_framebuffer = firmware_framebuffer_active ? 1u : 0u;
                read_host_bridge_memory_config(intel_alder_lake_id(device) ? 1u : 0u);
                gpu_ready = 1u;

                console_write("[ OK ] Intel graphics PCI function ");
                intel_gpu_hex32(device);
                console_write(" BAR0=0x");
                intel_gpu_hex32((uint32_t)(bar0 >> 32));
                intel_gpu_hex32((uint32_t)bar0);
                console_write(" size=");
                console_write_dec((uint32_t)(bar0_size / (1024u * 1024u)));
                console_write(" MiB\n");

                if (gpu.host_bridge_found) {
                    console_write("[ OK ] Intel host bridge memory map (read-only)\n");
                    console_write("       GGC=0x"); intel_gpu_hex32(gpu.host_ggc);
                    console_write(" BDSM=0x"); intel_gpu_hex32(gpu.host_bdsm);
                    console_write(" BGSM=0x"); intel_gpu_hex32(gpu.host_bgsm);
                    console_write(" TOLUD=0x"); intel_gpu_hex32(gpu.host_tolud);
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
                                intel_gpu_probe_ggtt();
                                intel_gpu_probe_engine_registers();
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

int intel_gpu_get_engine_snapshot(intel_gpu_engine_snapshot_t *out)
{
    if (!out || !engine_snapshot.valid)
        return -1;
    *out = engine_snapshot;
    return 0;
}

int intel_gpu_get_gt_diagnostics(intel_gt_diag_t *out)
{
    if (!out)
        return -1;
    *out = gt_diagnostics;
    return 0;
}

/* GGTT ownership remains unverified; this report never enables mappings. */
static uint32_t intel_gpu_ggtt_report_blockers(uint32_t aperture_ready,
                                                   uint32_t aperture_size)
{
    uint32_t blockers =
        INTEL_GGTT_BLOCKER_FIRMWARE_HANDOFF |
        INTEL_GGTT_BLOCKER_DISPLAY_RESERVATIONS |
        INTEL_GGTT_BLOCKER_PTE_CONTRACT |
        INTEL_GGTT_BLOCKER_CPU_GPU_COHERENCY |
        INTEL_GGTT_BLOCKER_INVALIDATION;

    if (!aperture_ready || !aperture_size || (aperture_size & 0xFFFu))
        blockers |= INTEL_GGTT_BLOCKER_APERTURE_UNAVAILABLE;
    return blockers;
}

int intel_gpu_ggtt_ownership_report_selftest(void)
{
    const uint32_t required =
        INTEL_GGTT_BLOCKER_FIRMWARE_HANDOFF |
        INTEL_GGTT_BLOCKER_DISPLAY_RESERVATIONS |
        INTEL_GGTT_BLOCKER_PTE_CONTRACT |
        INTEL_GGTT_BLOCKER_CPU_GPU_COHERENCY |
        INTEL_GGTT_BLOCKER_INVALIDATION;

    if (intel_gpu_ggtt_report_blockers(1u, 8u * 1024u * 1024u) != required ||
        intel_gpu_ggtt_report_blockers(0u, 0u) !=
            (required | INTEL_GGTT_BLOCKER_APERTURE_UNAVAILABLE) ||
        intel_gpu_ggtt_report_blockers(1u, 4097u) !=
            (required | INTEL_GGTT_BLOCKER_APERTURE_UNAVAILABLE))
        return -1;
    debug_write("LIONOS:INTEL-GGTT-OWNERSHIP-REPORT-SELFTEST-OK\n");
    return 0;
}

int intel_gpu_ggtt_get_ownership_report(intel_gpu_ggtt_ownership_report_t *out)
{
    if (!out || !gpu_ready)
        return -1;
    *out = (intel_gpu_ggtt_ownership_report_t){0};
    out->aperture_ready = gpu.ggtt_ready ? 1u : 0u;
    out->entry_count = gpu.ggtt_ready ? gpu.ggtt_size / 4096u : 0u;
    out->firmware_framebuffer_active = gpu.firmware_framebuffer ? 1u : 0u;
    out->evidence_mask = 0u;
    out->blocker_mask = intel_gpu_ggtt_report_blockers(
        gpu.ggtt_ready, gpu.ggtt_size);
    out->ownership_verified = 0u;
    out->mapping_permitted = 0u;
    return 0;
}


/* -------------------------------------------------------------------------
 * Intel GPU backing-memory manager (stage 1).
 *
 * This stage owns system-RAM pages and CPU mappings. GGTT entries are
 * scanned read-only; no GGTT slots are reserved. It deliberately does NOT write
 * GGTT PTEs, invalidate the GPU TLB, touch stolen memory, or program engines.
 * ------------------------------------------------------------------------- */
#define INTEL_GPU_GGTT_MAX_ENTRIES 8192u
#define INTEL_GPU_GGTT_BITMAP_WORDS (INTEL_GPU_GGTT_MAX_ENTRIES / 32u)
#define INTEL_GPU_MEMORY_MAX_ALLOCS 16u
#define INTEL_GPU_MEMORY_PAGE_SIZE 4096u

/*
 * Alder Lake uses the Gen8+ GGTT system-memory PTE format:
 * address[45:12] | Present. Bit 1 (LM) must remain clear for system RAM.
 * This encoder is software-only; it does not write the GGTT aperture.
 */
#define INTEL_GPU_GGTT_PTE_PRESENT       (1ULL << 0)
#define INTEL_GPU_GGTT_PTE_LOCAL_MEMORY  (1ULL << 1)
#define INTEL_GPU_GGTT_PTE_ADDRESS_MASK  0x00003FFFFFFFF000ULL

static int intel_gpu_ggtt_encode_system_pte(uint64_t physical_address,
                                             uint64_t *pte_out)
{
    if (!pte_out ||
        (physical_address & 0xFFFu) != 0u ||
        (physical_address & ~INTEL_GPU_GGTT_PTE_ADDRESS_MASK) != 0u)
        return -1;

    *pte_out = (physical_address & INTEL_GPU_GGTT_PTE_ADDRESS_MASK) |
               INTEL_GPU_GGTT_PTE_PRESENT;
    return 0;
}

int intel_gpu_ggtt_pte_selftest(void)
{
    static const uint64_t valid_addresses[] = {
        0x0000000010000000ULL,
        0x000000007C800000ULL,
        INTEL_GPU_GGTT_PTE_ADDRESS_MASK
    };
    static const uint64_t invalid_addresses[] = {
        0x0000000010000001ULL,
        0x0000400000000000ULL,
        0xFFFFFFFFFFFFF000ULL
    };

    for (uint32_t i = 0u;
         i < sizeof(valid_addresses) / sizeof(valid_addresses[0]); ++i) {
        uint64_t pte = 0u;
        uint64_t expected = valid_addresses[i] |
                            INTEL_GPU_GGTT_PTE_PRESENT;
        if (intel_gpu_ggtt_encode_system_pte(valid_addresses[i], &pte) != 0 ||
            pte != expected ||
            (pte & INTEL_GPU_GGTT_PTE_PRESENT) == 0u ||
            (pte & INTEL_GPU_GGTT_PTE_LOCAL_MEMORY) != 0u ||
            (pte & INTEL_GPU_GGTT_PTE_ADDRESS_MASK) != valid_addresses[i])
            goto fail;
    }

    for (uint32_t i = 0u;
         i < sizeof(invalid_addresses) / sizeof(invalid_addresses[0]); ++i) {
        uint64_t pte = 0xA5A5A5A5A5A5A5A5ULL;
        if (intel_gpu_ggtt_encode_system_pte(invalid_addresses[i], &pte) == 0 ||
            pte != 0xA5A5A5A5A5A5A5A5ULL)
            goto fail;
    }

    if (intel_gpu_ggtt_encode_system_pte(0x10000000ULL, 0) == 0)
        goto fail;

    debug_write("LIONOS:INTEL-GGTT-PTE-ENCODE-SELFTEST-OK\n");
    return 0;

fail:
    debug_write("LIONOS:INTEL-GGTT-PTE-ENCODE-SELFTEST-FAIL\n");
    return -1;
}

typedef struct {
    uint32_t state;       /* 0 free, 1 initializing, 2 active, 3 poisoned. */
    uint32_t generation;
    uint32_t page_count;
    uint32_t pin_count;
    uint32_t ggtt_first;
    uint32_t ggtt_reserved;
    uint64_t physical[INTEL_GPU_MEMORY_MAX_PAGES];
    uint32_t virtual_address[INTEL_GPU_MEMORY_MAX_PAGES];
} intel_gpu_memory_record_t;

static uint32_t intel_gpu_ggtt_bitmap[INTEL_GPU_GGTT_BITMAP_WORDS];
static uint32_t intel_gpu_ggtt_entries;
static uint32_t intel_gpu_ggtt_empty_candidates;
static uint32_t intel_gpu_ggtt_valid_entries;
static uint32_t intel_gpu_ggtt_nonzero_invalid_entries;
static uint32_t intel_gpu_memory_generation;
static uint32_t intel_gpu_memory_ready;
static intel_gpu_memory_record_t intel_gpu_memory_records[INTEL_GPU_MEMORY_MAX_ALLOCS];

/*
 * Read-only fingerprint of the complete GGTT aperture. This is a diagnostic
 * guard around CPU-side memory tests; it is not an ownership or mapping test.
 */
static int intel_gpu_ggtt_snapshot_crc32(uint32_t *crc_out)
{
    uint32_t crc = 0xFFFFFFFFu;

    if (!crc_out || !intel_gpu_memory_ready || !intel_gpu_ggtt_entries)
        return -1;

    for (uint32_t entry = 0u; entry < intel_gpu_ggtt_entries; ++entry) {
        uint64_t pte = 0u;

        if (intel_gpu_ggtt_read_entry(entry, &pte) != 0)
            return -1;

        for (uint32_t byte = 0u; byte < 8u; ++byte) {
            crc ^= (uint32_t)((pte >> (byte * 8u)) & 0xFFu);
            for (uint32_t bit = 0u; bit < 8u; ++bit) {
                uint32_t mask = 0u - (crc & 1u);
                crc = (crc >> 1) ^ (0xEDB88320u & mask);
            }
        }
    }

    *crc_out = ~crc;
    return 0;
}

static int intel_gpu_memory_bsp_only(void)
{
    return cpu_current_index() == 0u;
}

static int intel_gpu_ggtt_bit_get(uint32_t index)
{
    return (intel_gpu_ggtt_bitmap[index >> 5] & (1u << (index & 31u))) != 0u;
}

static void intel_gpu_ggtt_bit_set(uint32_t index)
{
    intel_gpu_ggtt_bitmap[index >> 5] |= 1u << (index & 31u);
}

int intel_gpu_memory_init(void)
{
    intel_gpu_info_t info;
    uint32_t entries;

    if (!intel_gpu_memory_bsp_only())
        return -1;

    /*
     * Initialization is intentionally idempotent. Re-scanning by clearing
     * the records would orphan live high-memory pages. GGTT entries are
     * diagnostic-only and are never reserved by this manager.
     */
    if (intel_gpu_memory_ready)
        return 0;

    if (!intel_gpu_is_alder_lake() ||
        intel_gpu_get_info(&info) != 0 || !info.ggtt_ready ||
        !info.ggtt_size || (info.ggtt_size & (INTEL_GPU_MEMORY_PAGE_SIZE - 1u)))
        return -1;

    entries = info.ggtt_size / INTEL_GPU_MEMORY_PAGE_SIZE;
    if (entries < 2u || entries > INTEL_GPU_GGTT_MAX_ENTRIES)
        return -1;

    intel_gpu_memory_ready = 0u;
    intel_gpu_ggtt_entries = entries;
    intel_gpu_ggtt_empty_candidates = 0u;
    for (uint32_t i = 0u; i < INTEL_GPU_GGTT_BITMAP_WORDS; ++i)
        intel_gpu_ggtt_bitmap[i] = 0u;
    for (uint32_t i = 0u; i < INTEL_GPU_MEMORY_MAX_ALLOCS; ++i)
        intel_gpu_memory_records[i].state = 0u;

    intel_gpu_ggtt_valid_entries = 0u;
    intel_gpu_ggtt_nonzero_invalid_entries = 0u;

    /*
     * Read every PTE. Only an exactly-zero PTE is an empty candidate.
     * Nonzero invalid PTEs are conservatively occupied: bit 0 alone is
     * not an ownership/free-space indicator.
     */
    for (uint32_t i = 0u; i < entries; ++i) {
        uint64_t pte = 0u;
        if (intel_gpu_ggtt_read_entry(i, &pte) != 0) {
            intel_gpu_ggtt_entries = 0u;
            debug_write("LIONOS:INTEL-GPU-MEM-GGTT-SCAN-FAIL\n");
            return -1;
        }
        if (i == 0u) {
            intel_gpu_ggtt_bit_set(i);
        } else if (pte == 0u) {
            ++intel_gpu_ggtt_empty_candidates;
        } else {
            intel_gpu_ggtt_bit_set(i);
            if ((pte & 1u) != 0u)
                ++intel_gpu_ggtt_valid_entries;
            else
                ++intel_gpu_ggtt_nonzero_invalid_entries;
        }
    }

    intel_gpu_memory_ready = 1u;
    debug_write("LIONOS:INTEL-GPU-MEM-MANAGER-READY\n");
    return 0;
}

int intel_gpu_memory_alloc(uint32_t page_count, intel_gpu_memory_block_t *out)
{
    intel_gpu_memory_record_t *record = 0;
    uint32_t slot = INTEL_GPU_MEMORY_MAX_ALLOCS;

    if (!out || !intel_gpu_memory_bsp_only() || !intel_gpu_memory_ready ||
        !page_count || page_count > INTEL_GPU_MEMORY_MAX_PAGES)
        return -1;

    for (uint32_t i = 0u; i < INTEL_GPU_MEMORY_MAX_ALLOCS; ++i) {
        if (intel_gpu_memory_records[i].state == 0u) {
            slot = i;
            record = &intel_gpu_memory_records[i];
            break;
        }
    }
    if (!record)
        return -1;

    /*
     * A zero PTE is only an observed empty candidate; it does not establish
     * ownership of that GGTT slot. Until platform documentation and firmware
     * hand-off provide an explicit ownership contract, do not reserve GGTT
     * entries or expose GPU virtual addresses. Backing RAM remains usable
     * through the CPU-only path.
     */
    record->state = 1u;
    intel_gpu_memory_generation = (intel_gpu_memory_generation + 1u) & 0x00FFFFFFu;
    if (!intel_gpu_memory_generation)
        intel_gpu_memory_generation = 1u;
    record->generation = intel_gpu_memory_generation;
    record->page_count = page_count;
    record->pin_count = 0u;
    for (uint32_t i = 0u; i < page_count; ++i) {
        record->physical[i] = 0u;
        record->virtual_address[i] = 0u;
    }
    record->ggtt_first = 0u;
    record->ggtt_reserved = 0u;

    for (uint32_t i = 0u; i < page_count; ++i) {
        uint64_t physical = 0u;
        uint32_t virtual_address = 0u;

        if (page_alloc_high(&physical) != 0)
            goto rollback;
        record->physical[i] = physical;

        if (paging_map_high_page(physical, 0x2u, &virtual_address) != 0)
            goto rollback;
        record->virtual_address[i] = virtual_address;

        /* The CPU mapping is writable and uncached policy is not imposed on RAM. */
        volatile uint32_t *words = (volatile uint32_t *)(uintptr_t)virtual_address;
        for (uint32_t word = 0u; word < INTEL_GPU_MEMORY_PAGE_SIZE / sizeof(uint32_t); ++word)
            words[word] = 0u;
    }

    record->state = 2u;
    out->handle = (record->generation << 8) | (slot + 1u);
    out->page_count = page_count;
    out->ggtt_first_entry = UINT32_MAX;
    out->bytes = page_count * INTEL_GPU_MEMORY_PAGE_SIZE;
    out->ggtt_mapping_active = 0u;
    return 0;

rollback:
    for (uint32_t i = 0u; i < page_count; ++i) {
        if (record->virtual_address[i]) {
            if (paging_unmap_high_page(record->virtual_address[i]) != 0) {
                record->state = 3u; /* Keep every backing page reserved on cleanup failure. */
                debug_write("LIONOS:INTEL-GPU-MEM-ROLLBACK-UNMAP-FAIL\n");
                return -1;
            }
            record->virtual_address[i] = 0u;
        }
        if (record->physical[i]) {
            page_free_high(record->physical[i]);
            record->physical[i] = 0u;
        }
    }
    record->state = 0u;
    record->page_count = 0u;
    record->ggtt_first = 0u;
    record->ggtt_reserved = 0u;
    return -1;
}

static intel_gpu_memory_record_t *intel_gpu_memory_lookup(uint32_t handle)
{
    uint32_t slot_code = handle & 0xFFu;
    uint32_t generation = handle >> 8;
    if (!slot_code || slot_code > INTEL_GPU_MEMORY_MAX_ALLOCS || !generation)
        return 0;
    intel_gpu_memory_record_t *record = &intel_gpu_memory_records[slot_code - 1u];
    if (record->state != 2u || record->generation != generation)
        return 0;
    return record;
}

int intel_gpu_memory_get_page(uint32_t handle, uint32_t page_index,
                              uint64_t *physical_address,
                              uint32_t *cpu_virtual_address)
{
    intel_gpu_memory_record_t *record;
    if (!intel_gpu_memory_bsp_only() || !physical_address || !cpu_virtual_address)
        return -1;
    record = intel_gpu_memory_lookup(handle);
    if (!record || page_index >= record->page_count)
        return -1;
    *physical_address = record->physical[page_index];
    *cpu_virtual_address = record->virtual_address[page_index];
    return 0;
}

/*
 * Build a software-only GGTT mapping candidate for one backing page.
 * Currently no slot is considered owned, so this routine fails closed.
 * It may return a plan only after a future platform ownership contract
 * explicitly permits reservation and mapping.
 */
int intel_gpu_memory_get_mapping_candidate(uint32_t handle,
                                           uint32_t page_index,
                                           uint32_t *ggtt_index,
                                           uint64_t *pte_value)
{
    intel_gpu_memory_record_t *record;
    uint32_t index;
    uint64_t encoded;
    uint64_t observed_pte = UINT64_MAX;

    if (!intel_gpu_memory_bsp_only() || !ggtt_index || !pte_value)
        return -1;

    record = intel_gpu_memory_lookup(handle);
    if (!record || !record->ggtt_reserved || page_index >= record->page_count)
        return -1;

    index = record->ggtt_first + page_index;
    if (index == 0u || index >= intel_gpu_ggtt_entries ||
        !intel_gpu_ggtt_bit_get(index) ||
        intel_gpu_ggtt_encode_system_pte(record->physical[page_index],
                                          &encoded) != 0)
        return -1;

    /*
     * Revalidate the hardware slot immediately before returning a plan.
     * A slot that was zero during the initial scan may since have been
     * populated by firmware or another agent. Never treat the software
     * reservation bitmap as proof that the hardware entry is still empty.
     *
     * This remains read-only validation, NOT proof of firmware ownership
     * and NOT authorization to write the PTE. A later mapping stage needs
     * an explicit platform ownership contract and serialization.
     */
    if (intel_gpu_ggtt_read_entry(index, &observed_pte) != 0 ||
        observed_pte != 0u)
        return -1;

    *ggtt_index = index;
    *pte_value = encoded;
    return 0;
}

int intel_gpu_memory_pin(uint32_t handle)
{
    intel_gpu_memory_record_t *record;

    if (!intel_gpu_memory_bsp_only())
        return -1;
    record = intel_gpu_memory_lookup(handle);
    if (!record || record->pin_count == UINT32_MAX)
        return -1;

    ++record->pin_count;
    return 0;
}

int intel_gpu_memory_unpin(uint32_t handle)
{
    intel_gpu_memory_record_t *record;

    if (!intel_gpu_memory_bsp_only())
        return -1;
    record = intel_gpu_memory_lookup(handle);
    if (!record || !record->pin_count)
        return -1;

    --record->pin_count;
    return 0;
}

int intel_gpu_memory_free(uint32_t handle)
{
    intel_gpu_memory_record_t *record;
    uint32_t count;

    if (!intel_gpu_memory_bsp_only())
        return -1;
    record = intel_gpu_memory_lookup(handle);
    if (!record || record->pin_count)
        return -1;

    count = record->page_count;

    /* Unmap all CPU aliases before returning any backing page to the PMM. */
    for (uint32_t i = 0u; i < count; ++i) {
        if (paging_unmap_high_page(record->virtual_address[i]) != 0) {
            record->state = 3u;
            debug_write("LIONOS:INTEL-GPU-MEM-FREE-UNMAP-FAIL\n");
            return -1;
        }
    }
    for (uint32_t i = 0u; i < count; ++i)
        page_free_high(record->physical[i]);

    record->state = 0u;
    record->page_count = 0u;
    record->pin_count = 0u;
    record->ggtt_first = 0u;
    record->ggtt_reserved = 0u;
    return 0;
}

int intel_gpu_memory_get_stats(intel_gpu_memory_stats_t *out)
{
    uint32_t allocations = 0u, pages = 0u;
    if (!out || !intel_gpu_memory_bsp_only() || !intel_gpu_memory_ready)
        return -1;
    for (uint32_t i = 0u; i < INTEL_GPU_MEMORY_MAX_ALLOCS; ++i) {
        if (intel_gpu_memory_records[i].state == 2u) {
            ++allocations;
            pages += intel_gpu_memory_records[i].page_count;
        }
    }
    out->ggtt_total_entries = intel_gpu_ggtt_entries;
    out->ggtt_empty_candidates = intel_gpu_ggtt_empty_candidates;
    out->ggtt_valid_entries = intel_gpu_ggtt_valid_entries;
    out->ggtt_nonzero_invalid_entries = intel_gpu_ggtt_nonzero_invalid_entries;
    out->active_allocations = allocations;
    out->allocated_pages = pages;
    out->initialized = intel_gpu_memory_ready;
    out->ggtt_ownership_verified = 0u;
    return 0;
}

int intel_gpu_memory_selftest(void)
{
    intel_gpu_memory_block_t block = {0};
    intel_gpu_memory_stats_t before = {0}, after = {0};
    uint64_t physical[2] = {0u, 0u};
    uint32_t virtual_address[2] = {0u, 0u};
    volatile uint32_t *words[2] = {0, 0};
    uint32_t handle = 0u;
    uint32_t pin_count = 0u;
    uint32_t ggtt_crc_before = 0u;
    uint32_t ggtt_crc_after = 0u;

    if (intel_gpu_memory_get_stats(&before) != 0 ||
        intel_gpu_ggtt_snapshot_crc32(&ggtt_crc_before) != 0 ||
        intel_gpu_memory_alloc(2u, &block) != 0)
        return -1;
    handle = block.handle;

    /* Active consumer pins must prevent free until every owner releases. */
    if (intel_gpu_memory_pin(handle) != 0)
        goto fail;
    ++pin_count;
    if (intel_gpu_memory_pin(handle) != 0)
        goto fail;
    ++pin_count;
    if (intel_gpu_memory_free(handle) == 0 ||
        intel_gpu_memory_unpin(handle) != 0)
        goto fail;
    --pin_count;
    if (intel_gpu_memory_free(handle) == 0 ||
        intel_gpu_memory_unpin(handle) != 0)
        goto fail;
    --pin_count;
    if (intel_gpu_memory_unpin(handle) == 0 ||
        intel_gpu_memory_get_page(handle, 0u, &physical[0],
                                  &virtual_address[0]) != 0)
        goto fail;

    if (block.page_count != 2u ||
        block.bytes != 2u * INTEL_GPU_MEMORY_PAGE_SIZE ||
        block.ggtt_mapping_active != 0u)
        goto fail;

    for (uint32_t page = 0u; page < 2u; ++page) {
        if (intel_gpu_memory_get_page(handle, page, &physical[page],
                                      &virtual_address[page]) != 0)
            goto fail;
        if ((physical[page] & (INTEL_GPU_MEMORY_PAGE_SIZE - 1u)) != 0u ||
            (virtual_address[page] & (INTEL_GPU_MEMORY_PAGE_SIZE - 1u)) != 0u)
            goto fail;

        /*
         * Validate the actual PMM-provided backing address, not only fixed
         * test vectors. This remains a software-only check: no GGTT write.
         */
        uint64_t encoded_pte = 0u;
        if (intel_gpu_ggtt_encode_system_pte(physical[page], &encoded_pte) != 0 ||
            (encoded_pte & INTEL_GPU_GGTT_PTE_PRESENT) == 0u ||
            (encoded_pte & INTEL_GPU_GGTT_PTE_LOCAL_MEMORY) != 0u ||
            (encoded_pte & INTEL_GPU_GGTT_PTE_ADDRESS_MASK) != physical[page])
            goto fail;

        uint32_t candidate_index = 0xA5A5A5A5u;
        uint64_t candidate_pte = 0xA5A5A5A5A5A5A5A5ULL;
        if (block.ggtt_first_entry == UINT32_MAX) {
            /* GGTT ownership is unverified; backing RAM is CPU-only. */
            if (intel_gpu_memory_get_mapping_candidate(handle, page,
                                                       &candidate_index,
                                                       &candidate_pte) == 0 ||
                candidate_index != 0xA5A5A5A5u ||
                candidate_pte != 0xA5A5A5A5A5A5A5A5ULL)
                goto fail;
        } else if (intel_gpu_memory_get_mapping_candidate(handle, page,
                                                           &candidate_index,
                                                           &candidate_pte) != 0 ||
                   candidate_index != block.ggtt_first_entry + page ||
                   candidate_pte != encoded_pte) {
            goto fail;
        }

        words[page] = (volatile uint32_t *)(uintptr_t)virtual_address[page];
    }

    if (physical[0] == physical[1] ||
        virtual_address[0] == virtual_address[1])
        goto fail;

    {
        uint32_t invalid_index = 0xA5A5A5A5u;
        uint64_t invalid_pte = 0xA5A5A5A5A5A5A5A5ULL;

        /* The first out-of-range page and both null-output cases must fail
           without modifying any caller-owned output storage. */
        if (intel_gpu_memory_get_mapping_candidate(handle, block.page_count,
                                                   &invalid_index,
                                                   &invalid_pte) == 0 ||
            invalid_index != 0xA5A5A5A5u ||
            invalid_pte != 0xA5A5A5A5A5A5A5A5ULL)
            goto fail;

        if (intel_gpu_memory_get_mapping_candidate(handle, 0u, 0,
                                                   &invalid_pte) == 0 ||
            invalid_pte != 0xA5A5A5A5A5A5A5A5ULL)
            goto fail;

        if (intel_gpu_memory_get_mapping_candidate(handle, 0u,
                                                   &invalid_index, 0) == 0 ||
            invalid_index != 0xA5A5A5A5u)
            goto fail;
    }
    for (uint32_t page = 0u; page < 2u; ++page) {
        for (uint32_t i = 0u;
             i < INTEL_GPU_MEMORY_PAGE_SIZE / sizeof(uint32_t); ++i) {
            words[page][i] = 0x4C494F4Eu ^
                ((page + 1u) * 0x85EBCA6Bu) ^ (i * 0x9E3779B9u);
        }
    }

    for (uint32_t page = 0u; page < 2u; ++page) {
        for (uint32_t i = 0u;
             i < INTEL_GPU_MEMORY_PAGE_SIZE / sizeof(uint32_t); ++i) {
            uint32_t expected = 0x4C494F4Eu ^
                ((page + 1u) * 0x85EBCA6Bu) ^ (i * 0x9E3779B9u);
            if (words[page][i] != expected)
                goto fail;
        }
    }

    if (intel_gpu_memory_free(handle) != 0)
        goto fail;
    if (intel_gpu_memory_get_page(handle, 0u, &physical[0],
                                  &virtual_address[0]) == 0)
        goto fail;
    {
        uint32_t stale_index = 0xA5A5A5A5u;
        uint64_t stale_pte = 0xA5A5A5A5A5A5A5A5ULL;
        if (intel_gpu_memory_get_mapping_candidate(handle, 0u,
                                                   &stale_index,
                                                   &stale_pte) == 0 ||
            stale_index != 0xA5A5A5A5u ||
            stale_pte != 0xA5A5A5A5A5A5A5A5ULL)
            goto fail;
    }
    if (intel_gpu_ggtt_snapshot_crc32(&ggtt_crc_after) != 0 ||
        ggtt_crc_after != ggtt_crc_before ||
        intel_gpu_memory_get_stats(&after) != 0 ||
        after.active_allocations != before.active_allocations ||
        after.allocated_pages != before.allocated_pages ||
        after.ggtt_empty_candidates != before.ggtt_empty_candidates ||
        after.ggtt_valid_entries != before.ggtt_valid_entries ||
        after.ggtt_nonzero_invalid_entries != before.ggtt_nonzero_invalid_entries ||
        before.ggtt_ownership_verified != 0u ||
        after.ggtt_ownership_verified != 0u)
        goto fail;

    debug_write("LIONOS:INTEL-GPU-GGTT-SNAPSHOT-STABLE\n");
    debug_write("LIONOS:INTEL-GPU-MEM-2PAGE-SELFTEST-OK\n");
    return 0;

fail:
    while (pin_count) {
        if (intel_gpu_memory_unpin(handle) != 0)
            break;
        --pin_count;
    }
    (void)intel_gpu_memory_free(handle);
    debug_write("LIONOS:INTEL-GPU-MEM-2PAGE-SELFTEST-FAIL\n");
    return -1;
}
