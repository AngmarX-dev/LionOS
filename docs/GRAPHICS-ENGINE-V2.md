# LionOS Graphics Engine v2

## Scope

This branch separates desktop frame pacing from the process scheduler and provides an ARGB software-compositor layer path. Native GPU command submission is not enabled by this milestone.

## CPU software rasterizer primitives

The hardware-independent graphics API now provides a bounded CPU path for clearing a surface, writing a pixel, filling a clipped rectangle, drawing an in-bounds line, and filling a triangle. ARGB8888 preserves the supplied alpha byte; XRGB8888 always clears the high byte. All operations validate the context and respect the surface pitch, leaving row padding untouched.

The filled-triangle routine uses signed 64-bit edge functions, clips its iteration to the triangle's in-bounds bounding box, accepts either vertex winding, and treats degenerate triangles as a no-op. All three vertices must be inside the target surface; a triangle with any out-of-bounds vertex is rejected without writing. Its deterministic boot self-test covers ARGB and XRGB output, winding-independent coverage, degenerate/out-of-bounds handling, and row-padding preservation.

This is CPU rasterization only. It does not submit GPU commands or imply hardware acceleration.

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

### Implemented: GPU backing-memory manager (stage 1)

The first GPU memory-manager stage is implemented for the matched Alder Lake path:

1. Scan every GGTT PTE through the bounded, read-only accessor. If any entry cannot be read consistently, initialization fails closed.
2. Classify entry zero as reserved; count nonzero valid PTEs, nonzero invalid PTEs, and exactly-zero observations separately. A zero PTE is only an empty observation, not proof that firmware or another agent has relinquished the slot.
3. Do not reserve any GGTT slot. Without a documented platform hand-off/ownership contract, the driver treats every zero entry as unowned and unavailable for allocation. The old software range allocator has been removed.
4. Allocate backing pages from the high physical-memory allocator (256 MiB..64 GiB), map each page into the CPU high-memory window, and zero it before exposing the allocation. These buffers are CPU-accessible system RAM only.
5. Keep per-allocation physical and CPU virtual page addresses and validate generation-tagged handles. CPU mappings/pages are released in a controlled order; no GGTT range is reserved or released.
6. Keep the mapping-candidate API fail-closed. It cannot return a GGTT index/PTE plan until an explicit ownership contract is implemented and validated.
7. On allocation failure, roll back successfully created CPU mappings and physical pages. If unmapping fails, poison the record and retain its backing pages rather than risk freeing memory still mapped by the CPU.
8. Run the two-page allocate/map/write/read/unmap/free CPU-side self-test during boot. Verify that GGTT ownership remains unverified and that the GGTT snapshot is unchanged.

**Important boundary:** this stage does not write GGTT PTEs, create GPU virtual mappings, touch stolen memory, or make allocations GPU-addressable. The ggtt_mapping_active field therefore remains zero. The firmware framebuffer and CPU compositor remain the only active display path. The manager is BSP-only and externally serialized.

### Implemented: software-only GGTT mapping candidates

The mapping-candidate API remains present for the future provider contract, but it currently fails closed because LionOS has not established ownership of any GGTT entry. It leaves output parameters unchanged. The backing-memory self-test confirms that CPU-side allocations work while no GGTT slot is claimed.

A zero-valued PTE is not an ownership token. The software allocator for zero-valued GGTT entries has therefore been disabled and removed. Runtime stats expose `ggtt_ownership_verified = 0`, and boot diagnostics explicitly report that GPU virtual mappings are disabled.

### Implemented: software-only Gen8+ GGTT PTE encoder validation

LionOS now has a pure software encoder for system-RAM GGTT PTE candidates. It follows the generic Gen8+ encoding used by Linux i915 for Alder Lake: physical address bits 45:12 plus Present bit 0. The Local Memory bit (bit 1) is deliberately clear because these backing pages come from system RAM, not local device memory.

The encoder rejects unaligned physical addresses and addresses outside the GGTT PTE address field. Its boot self-test checks valid lower/upper boundary addresses, rejects misaligned and out-of-range addresses, verifies the Present/address fields, verifies Local Memory remains clear, and confirms failed calls do not modify the caller's output value. The two-page backing-memory self-test also passes each physical address returned by the real high-memory allocator through the encoder and verifies the resulting Present, address, and Local Memory fields.

**Safety boundary:** this is encoding validation only. The test does not write a GGTT PTE, change firmware mappings, perform invalidation, or make any buffer GPU-addressable. It does not establish ownership of an empty GGTT entry.

Reference: Linux i915 `gen8_ggtt_pte_encode()` and `GEN12_GGTT_PTE_ADDR_MASK` / `GEN12_GGTT_PTE_LM` in `gt/intel_ggtt.c` and `gt/intel_gtt.h`.

### Not implemented yet

GGTT PTE programming and TLB invalidation, stolen-memory suballocation, long-lived GT power management, ring-buffer/engine initialization, context setup, command submission, fences, and hardware acceleration remain disabled. A successful memory self-test proves CPU-side allocation and mapping only; it does not prove GPU access.

### Next milestones

1. Validate the read-only memory-manager scan and CPU-side self-test on the physical Alder Lake laptop.
2. Establish an authoritative GGTT ownership/firmware hand-off contract for the exact platform, including reserved ranges and arbitration with display firmware.
3. Cross-check the exact Alder Lake PTE format, cache policy, invalidation sequence, and synchronization requirements against authoritative platform documentation and Linux i915.
4. Only after ownership is proven, implement a separately reviewed GGTT mapping/unmapping layer with transactional rollback.
5. Only after mapping is validated, initialize a bounded RCS0 ring and context, then add a minimal command test with bounded completion/fault handling and CPU fallback.
6. Measure correctness, frame time and CPU load before enabling acceleration by default.

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

### GPU memory manager safety contract

The manager exposes intel_gpu_memory_init, intel_gpu_memory_alloc, intel_gpu_memory_get_page, intel_gpu_memory_get_mapping_candidate, intel_gpu_memory_free, intel_gpu_memory_get_stats, and intel_gpu_memory_selftest in include/intel_gpu.h. Allocation sizes are page-granular and capped at 256 pages per block; the static descriptor pool is capped at 16 blocks. The handle encodes a generation and pool slot to reject stale references. Operations are BSP-only because the existing high-page mapping window is BSP-owned; callers must serialize access. No GGTT ranges are returned or reserved: all allocations remain CPU-only until GGTT ownership is explicitly verified. No code in this stage writes the GGTT aperture.
