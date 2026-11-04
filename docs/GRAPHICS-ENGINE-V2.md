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

The legacy identity-mapped PMM still manages the low 256 MiB. A separate high-physical-page allocator now tracks Multiboot2 available pages from 256 MiB up to a conservative 64 GiB ceiling with separate availability/allocation bitmaps. Non-available Multiboot2 regions, the Multiboot information block, and modules are excluded. High allocations are returned as 64-bit physical addresses and must never be dereferenced as 32-bit pointers; they must be mapped through the dedicated high-page virtual window. For a matched Alder Lake GPU, the bootstrap conservatively removes the complete validated BGSM..TOLUD interval from the allocator. The allocator must still not be used for GPU-owned memory or GGTT entries until the exact stolen-memory and GGTT encodings are validated.

### Alder Lake GT forcewake and engine-register snapshot

For the verified Alder Lake display device, the bootstrap uses the documented Gen9+ GT and render forcewake request/ACK registers, then samples the render (RCS0, base 0x2000) and blitter (BCS0, base 0x22000) ring HEAD, TAIL, START, and CONTROL registers. Forcewake is acquired only for this bounded diagnostic read and released afterward; LionOS does not reset the GPU, change display mode, program ring state, allocate GPU memory, write a ring, or submit commands.

The GMD_ID_GRAPHICS register (0xD8C) is not used as a GT-presence test on Alder Lake. Linux i915 gates GMD-ID reads behind the `HAS_GMD_ID` capability; that capability is not enabled for Alder Lake and is enabled on newer platforms such as Meteor Lake. Accordingly, `graphics_gmd_id_valid` remains false for this Alder Lake path. A zero GMD-ID value is therefore not a bring-up failure and must not prevent the render forcewake and ring-register diagnostics from running.

The ring values are raw diagnostic snapshots; nonzero or changing values alone do not prove that an engine is initialized or operational. This step validates register access only. It does not enable GPU acceleration or command submission.

Register offsets and capability handling are cross-checked against Linux i915 definitions in `gt/intel_gt_regs.h`, `gt/intel_engine_regs.h`, `i915_reg.h`, and the `HAS_GMD_ID` checks in `intel_device_info.c` / `i915_pci.c`. The exact Alder Lake platform specification remains authoritative.

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

The Intel graphics bootstrap now checks PCI function 00:00.0 for an Intel host bridge (class 06:00) and, when present, records the raw values of GGC (0x50), BDSM (0xB4), BGSM (0xB8), and TOLUD (0xBC). The raw values are diagnostic snapshots. For a matched Alder Lake display device only, if the masked BGSM, BDSM, and TOLUD candidates satisfy the expected increasing order, LionOS conservatively reserves the entire BGSM..TOLUD interval from the high-memory allocator. It does not infer individual stolen-memory sizes from the address spans or alter GGTT state. This conservative reservation must still be validated against the exact Intel 600-series/Alder Lake platform documentation before GPU memory use.

### Read-only memory-boundary validation

The hardware report now extracts the raw GGC fields using the documented field positions used by the Linux i915 register definitions: GMS is bits 15:8 and GGMS is bits 7:6. LionOS displays these field values without converting them to memory sizes until the exact Alder Lake platform encoding is confirmed.

For the host-bridge BDSM, BGSM, and TOLUD values, the report displays a 1 MiB-granularity base candidate using `value & 0xFFF00000`. It then checks only the ordering `BGSM < BDSM < TOLUD` and, if valid, displays the address spans in MiB. These spans are address differences, not claims about the actual allocated sizes of stolen memory or GGTT. No memory is reserved, mapped, cleared, or written by this diagnostic.

Reference for the GGC field positions: [Linux i915 register definitions](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/i915_reg.h) (`GMS_MASK` bits 15:8; `GGMS_MASK` bits 7:6). The exact Alder Lake datasheet remains authoritative for size encodings and platform-specific interpretation.

### Read-only Alder Lake GGTT aperture inspection

For the verified Alder Lake display path, the diagnostic bootstrap identifies the Gen8+ GTTMMADR layout only when BAR0 is exactly 16 MiB and the Intel host bridge was detected. The GGTT PTE aperture starts at BAR0 + 8 MiB. GGMS is decoded from GGC bits 7:6; encodings 1, 2, and 3 describe 2, 4, and 8 MiB respectively (encoding 0 disables the aperture). The resulting aperture is accepted only when its end remains within BAR0.

The driver exposes a bounded read-only accessor for 64-bit Gen8+ GGTT entries. It samples each entry with a bounded high-low-high sequence, retries at most three times if the high dword changes, and never writes a PTE or performs a GGTT invalidate. The hardware report displays the aperture offset/size and the first four PTE values as raw diagnostics only. No interpretation of those PTEs as owned allocations is made yet.

The GTTMMADR layout and GGTT offset follow the Linux i915 implementation: `drivers/gpu/drm/i915/gt/intel_ggtt.c` (`gen6_gttmmadr_size()` and `gen6_gttadr_offset()`). GGMS size decoding follows `gen8_get_total_gtt_size()`. This is a cross-check reference; the Intel Alder Lake platform documentation remains authoritative.

## High-physical memory management

The high-memory allocator tracks firmware-available pages from 256 MiB to 64 GiB, excludes non-available memory-map entries and Multiboot2 metadata/modules, and returns 64-bit physical addresses. A separate 48 MiB virtual window (`0xF1000000..0xF3FFFFFF)) maps one high physical page at a time; it is kept separate from the framebuffer window at `0xF4000000`, Intel GPU BAR mapping at `0xF8000000`, and LAPIC mapping. Mapping/unmapping is BSP-only and uses a dedicated APIC IPI (vector 49) to invalidate the virtual address on every online CPU before the operation returns.

The boot self-test allocates one high page, maps it, writes and verifies 4 KiB of test patterns, unmaps it, and frees it. The GUI reports currently free high RAM. This is a generic system-RAM allocator, not a GPU memory allocator: the exact Intel Alder Lake stolen-memory and GGTT reservations still need to be decoded and excluded before GPU-visible allocations are permitted.

### PAE mapping and synchronization safeguards

The kernel mapping API rejects physical addresses that cannot be represented by the PAE PTE address field. Shared kernel mappings are serialized and restricted to the BSP. After updating a PTE, the BSP invalidates its local TLB, sends vector 49 to online APs, and waits for their acknowledgements. The interrupt handler performs `invlpg`, increments the shared acknowledgement counter, and sends LAPIC EOI. This protocol is currently exercised by the high-memory boot self-test; QEMU CI does not replace a physical-hardware test.
