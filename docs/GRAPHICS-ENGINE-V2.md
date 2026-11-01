# LionOS Graphics Engine v2

## Scope

This branch separates desktop frame pacing from the process scheduler and provides an ARGB software-compositor layer path. Native GPU command submission is not enabled by this milestone.

## Intel UHD Graphics: Alder Lake

The first physical GPU target is the integrated Intel graphics function reported by the user's laptop:

- PCI vendor/device: `8086:468B`
- Revision: `0C`
- PCI location: `00:02.0`
- Firmware-reported BAR0 base: `0x0000006202000000`

The device ID is used for platform matching; the CPU marketing name alone is not used to infer the GPU SKU.

### Implemented bootstrap

1. Locate the Intel display-class PCI function.
2. Verify BAR0 is a 64-bit memory BAR.
3. Temporarily disable PCI memory decoding while sizing BAR0, then restore the original BAR pair and command register.
4. Reject zero, misaligned, non-power-of-two, or oversized BAR resources. The current implementation caps the mapped aperture at 64 MiB.
5. Map the validated physical aperture into the kernel PAE MMIO window at `0xF8000000` using uncached page attributes.
6. Expose a bounds-checked, read-only 32-bit MMIO accessor.

The firmware framebuffer remains the active display path. No GPU reset, display mode change, engine command, or undocumented register write is issued.

The current physical memory manager has a fixed 65,536-page bitmap and therefore manages only physical addresses below 256 MiB. The reported Alder Lake graphics boundaries (BGSM/BDSM/TOLUD) are around 2 GiB on the verified laptop, outside that managed range. LionOS must not add these graphics ranges to the allocator or treat them as free memory. A future PMM expansion must first consume the complete firmware memory map, preserve non-available regions, reserve the graphics stolen-memory/GGTT regions using the exact platform definitions, and handle address-width/overflow checks.

### Not implemented yet

This is a safe MMIO bootstrap, not a functioning acceleration engine. It does not yet initialize the GT, forcewake domains, GGTT, stolen memory, ring buffers, BLT engine, or display engine. Consequently the compositor continues to render on the CPU.

### Next milestones

1. Validate BAR sizing and MMIO mapping on the physical Alder Lake laptop.
2. Add documented, non-destructive register reads for the exact device generation.
3. Discover firmware memory reservations, stolen memory and GGTT aperture without changing ownership.
4. Implement a bounded BLT submission path with explicit GPU-visible memory allocation, fences/timeouts and CPU fallback.
5. Route only verified opaque copy/fill operations from the compositor to the GPU.
6. Measure correctness, frame time and CPU load on physical hardware before enabling acceleration by default.

### Compatibility constraints

- Keep the firmware framebuffer active until native display initialization is independently validated.
- No guessed BAR sizes, undocumented register writes, GPU reset, or unbounded waits.
- Do not modify xHCI, USB keyboard, USB mouse, or other input code as part of this work.
- QEMU build/boot tests cannot establish physical Intel GPU command completion.

## CPU topology reporting

The CPU diagnostic panel does not calculate physical core count from the difference between CPUID topology shift widths. Those widths describe x2APIC ID layout and are not a reliable physical-core count on hybrid processors.

For the verified Intel Core i5-12450HX (GenuineIntel, family 6, model 151, matching the exact brand string), the report uses its known 8-core SKU configuration. The logical processor count continues to come from CPUID topology enumeration. For other processor models without a reliable topology source or explicit verified mapping, the panel displays `Unknown` instead of a guessed count.

### Read-only host-bridge memory snapshot

The Intel graphics bootstrap now checks PCI function 00:00.0 for an Intel host bridge (class 06:00) and, when present, records the raw values of GGC (0x50), BDSM (0xB4), BGSM (0xB8), and TOLUD (0xBC). These are diagnostic snapshots only. LionOS does not decode them into physical allocations, reserve or release memory, change graphics memory ownership, or alter GGTT state in this stage. The values must be checked against the exact Intel 600-series/Alder Lake platform documentation before any memory manager integration.

### Read-only memory-boundary validation

The hardware report now extracts the raw GGC fields using the documented field positions used by the Linux i915 register definitions: GMS is bits 15:8 and GGMS is bits 7:6. LionOS displays these field values without converting them to memory sizes until the exact Alder Lake platform encoding is confirmed.

For the host-bridge BDSM, BGSM, and TOLUD values, the report displays a 1 MiB-granularity base candidate using `value & 0xFFF00000`. It then checks only the ordering `BGSM < BDSM < TOLUD` and, if valid, displays the address spans in MiB. These spans are address differences, not claims about the actual allocated sizes of stolen memory or GGTT. No memory is reserved, mapped, cleared, or written by this diagnostic.

Reference for the GGC field positions: [Linux i915 register definitions](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/i915_reg.h) (`GMS_MASK` bits 15:8; `GGMS_MASK` bits 7:6). The exact Alder Lake datasheet remains authoritative for size encodings and platform-specific interpretation.

### Read-only Alder Lake GGTT aperture inspection

For the verified Alder Lake display path, the diagnostic bootstrap identifies the Gen8+ GTTMMADR layout only when BAR0 is exactly 16 MiB and the Intel host bridge was detected. The GGTT PTE aperture starts at BAR0 + 8 MiB. GGMS is decoded from GGC bits 7:6; encodings 1, 2, and 3 describe 2, 4, and 8 MiB respectively (encoding 0 disables the aperture). The resulting aperture is accepted only when its end remains within BAR0.

The driver exposes a bounded read-only accessor for 64-bit Gen8+ GGTT entries. It samples each entry with a bounded high-low-high sequence, retries at most three times if the high dword changes, and never writes a PTE or performs a GGTT invalidate. The hardware report displays the aperture offset/size and the first four PTE values as raw diagnostics only. No interpretation of those PTEs as owned allocations is made yet.

The GTTMMADR layout and GGTT offset follow the Linux i915 implementation: `drivers/gpu/drm/i915/gt/intel_ggtt.c` (`gen6_gttmmadr_size()` and `gen6_gttadr_offset()`). GGMS size decoding follows `gen8_get_total_gtt_size()`. This is a cross-check reference; the Intel Alder Lake platform documentation remains authoritative.
