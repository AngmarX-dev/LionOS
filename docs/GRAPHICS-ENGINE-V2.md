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

The high-memory allocator tracks firmware-available pages from 256 MiB to 64 GiB, excludes non-available memory-map entries and Multiboot2 metadata/modules, and returns 64-bit physical addresses. A separate 32 MiB virtual window (`0xF2000000..0xF3FFFFFF`) maps one high physical page at a time. The preceding `0xF1000000..0xF10FFFFF` region is reserved for xHCI MMIO and is not part of the high-memory window. The high-memory window is kept separate from the framebuffer window at `0xF4000000`, Intel GPU BAR mapping at `0xF8000000`, and LAPIC mapping. Mapping/unmapping is BSP-only and uses a dedicated APIC IPI (vector 49) to invalidate the virtual address on every online CPU before the operation returns.

The boot self-test allocates one high page, maps it, writes and verifies 4 KiB of test patterns, unmaps it, and frees it. The GUI reports currently free high RAM. This is a generic system-RAM allocator, not a GPU memory allocator: the exact Intel Alder Lake stolen-memory and GGTT reservations still need to be decoded and excluded before GPU-visible allocations are permitted.

### PAE mapping and synchronization safeguards

The kernel mapping API rejects physical addresses that cannot be represented by the PAE PTE address field. Shared kernel mappings are serialized and restricted to the BSP. After updating a PTE, the BSP invalidates its local TLB, sends vector 49 to online APs, and waits for their acknowledgements. The interrupt handler performs `invlpg`, increments the shared acknowledgement counter, and sends LAPIC EOI. This protocol is currently exercised by the high-memory boot self-test; QEMU CI does not replace a physical-hardware test.

### GPU memory manager safety contract

The manager exposes intel_gpu_memory_init, intel_gpu_memory_alloc, intel_gpu_memory_get_page, intel_gpu_memory_get_mapping_candidate, intel_gpu_memory_free, intel_gpu_memory_get_stats, and intel_gpu_memory_selftest in include/intel_gpu.h. Allocation sizes are page-granular and capped at 256 pages per block; the static descriptor pool is capped at 16 blocks. The handle encodes a generation and pool slot to reject stale references. Operations are BSP-only because the existing high-page mapping window is BSP-owned; callers must serialize access. No GGTT ranges are returned or reserved: all allocations remain CPU-only until GGTT ownership is explicitly verified. No code in this stage writes the GGTT aperture.


### GGTT ownership-contract research checkpoint

The next GGTT stage is a documented no-go decision, not PTE programming. Current upstream Linux i915 code was cross-checked for Gen8+ PTE fields, platform-specific PAT handling, write/readback behavior, and generation-/GuC-dependent invalidation. This is implementation evidence, not proof of firmware hand-off or an authoritative Alder Lake platform contract.

The exact ownership, firmware scanout reservations, Alder Lake cacheability/coherency policy, PTE update ordering, and invalidation completion semantics remain unverified. Therefore the runtime must continue reporting ownership unverified, must not reserve zero-valued entries, and must not write GGTT PTEs.

The evidence, unresolved requirements, transactional map/unmap rules, and go/no-go checklist are maintained in [INTEL-ALDER-LAKE-GGTT-CONTRACT.md](INTEL-ALDER-LAKE-GGTT-CONTRACT.md). The next safe step is to obtain and validate the exact platform/firmware documentation; if no valid hand-off contract exists, GGTT mapping remains disabled.

## PMM / PAE mapping lifecycle audit (2026-11-05)

The current boot order was checked across `src/kernel.c`, `src/memory.c`,
`src/paging.c`, and `src/intel_gpu.c`:

1. `memory_init()` builds the low-memory allocator and the separate high-physical
   availability bitmap from the Multiboot2 memory map. The high allocator tracks
   the range 256 MiB..64 GiB; its returned values are physical addresses and are
   not directly dereferenceable.
2. `paging_init()` installs the low identity map and the 256 MiB kernel MMIO
   virtual window. The temporary high-page CPU mapping aperture is the separate
   32 MiB range `0xF2000000..0xF3FFFFFF`.
3. After PCI enumeration, `intel_gpu_init()` validates and reserves the Intel
   BAR0 physical aperture when its end-address calculation is valid. For a
   matched Alder Lake display ID, it also reads the host-bridge GGC/BDSM/BGSM/
   TOLUD registers and reserves the conservative BGSM..TOLUD interval only
   when the masked boundaries satisfy
   `BGSM < BDSM < TOLUD <= 4 GiB`.
4. Only after `intel_gpu_init()` reports a usable Intel MMIO aperture does
   `kernel_main()` initialize the GPU backing-memory manager and run its
   CPU-side allocation/mapping self-test.

This order means the currently implemented Intel reservation calls occur before
the Intel backing-memory self-test can allocate high pages. The reservation API
is not a retroactive ownership-transfer mechanism: future callers must reserve
firmware/device ranges before allocating from those ranges. The current
BGSM..TOLUD rule is a conservative exclusion based on register snapshots, not
proof that the driver owns or may modify stolen memory.

The high-memory diagnostic deliberately distinguishes raw Multiboot2 available
RAM inventory (`memory_high_available_bytes()`) from currently free high pages
(`memory_high_free_pages()`); reservations and allocations affect the latter,
not the former.

The kernel-wide mapping path serializes BSP mapping changes and waits for TLB
invalidation acknowledgements from the online APs selected by the current SMP
topology. It has no timeout: returning and reusing a virtual slot before every
target has invalidated its stale translation would be unsafe. A non-responding
online AP can therefore stall the BSP. This remains a reliability limitation
for a future SMP lifecycle review; this graphics milestone does not weaken the
acknowledgement requirement or alter the shootdown protocol.

**Result:** the inspected boot order supports the current CPU-only high-memory
backing allocation path. It does not establish GGTT ownership, GPU addressability,
or permission to submit commands. No GPU acceleration claim follows from this
audit.

## SMP / kernel TLB shootdown lifecycle audit (2026-11-05)

The shared kernel mapping path and its AP interrupt path were reviewed across
`src/paging.c`, `src/smp.c`, `src/cpu.c`, `src/lapic.c`,
`src/idt.c`, `src/spinlock.c`, and `boot/boot.asm`.

### Verified ordering and synchronization

1. The BSP initializes the shared IDT, LAPIC, paging structures, and SMP
   topology before AP startup. Each AP loads the shared IDT and enables its
   local APIC before enabling interrupts and publishing its `online` flag.
2. IDT vector 49 is installed as an interrupt gate. The common assembly stub
   preserves the interrupted register/segment frame, and
   `interrupt_dispatch()` routes vector 49 to
   `paging_tlb_shootdown_interrupt()`.
3. The handler reads the published virtual address, executes local
   `invlpg`, increments the acknowledgement counter with release ordering,
   and sends LAPIC EOI. It does not acquire the kernel mapping lock, so it
   does not depend on a lock held by the waiting BSP.
4. The BSP mapping transaction is restricted to CPU index zero and serialized
   by `kernel_map_lock` with local interrupts disabled. It updates the PTE,
   invalidates locally, snapshots the number of eligible online AP targets,
   publishes the address, sends vector 49 to those APs, and waits for the
   acknowledgement count before releasing the lock or returning.
5. The high-page unmap path clears the PTE and completes the same shootdown
   before returning the virtual slot to the high-map bitmap. This ordering
   prevents a slot from being reused while an online AP may retain its old
   translation.
6. The AP online flag is published only after its IDT and LAPIC are ready and
   local interrupts have been enabled. Thus a CPU is not included in the
   shootdown target set before it can service the IPI.

### Limitations and follow-up requirements

- The shootdown protocol has no transaction timeout or recovery path. A target
  that is marked online but cannot service vector 49 can stall the BSP
  indefinitely. This is fail-closed with respect to stale-translation reuse,
  but it is not fault tolerant.
- `lapic_send_ipi()` has a void return contract. Its bounded ICR wait does
  not report whether delivery completed; the acknowledgement loop is the
  eventual end-to-end check, but currently has no deadline or diagnostic
  escalation.
- The target count and send pass independently inspect the online topology.
  The current SMP implementation has no CPU-offline transition, so the online
  set is effectively monotonic after startup. Any future hotplug/offline
  support must make target selection an immutable per-transaction snapshot
  and coordinate CPU removal with in-flight shootdowns.
- `cpu_current_index()` returns BSP index zero when the current AP is not yet
  marked online. The present AP startup calls
  `paging_switch_address_space(paging_kernel_directory())` during that
  interval; it loads the same kernel CR3 and therefore does not change the
  BSP's address-space value. However, the fallback is not a general per-CPU
  identity mechanism. Future AP initialization must not use per-CPU state
  before the AP can be identified independently of its online state.
- APIC IDs above 255 are not started by the current xAPIC-only startup path,
  and the shootdown target filter likewise accepts only IDs up to 255.
  Supporting x2APIC or wider IDs requires a separate LAPIC/IPI implementation,
  not merely removing the filter.
- This audit does not validate interrupt delivery on physical multi-socket
  hardware. QEMU CI validates build/test regressions but is not a substitute
  for a physical SMP stress test.

**Result:** the current BSP/AP ordering and acknowledgement protocol are
internally consistent for the supported online xAPIC CPUs. Mapping removal
continues to wait for every selected AP acknowledgement before a virtual slot
can be reused. No timeout, CPU hotplug, x2APIC support, or GPU mapping behavior
is claimed. No xHCI source was modified by this audit.

## AP identity during CPU bring-up audit (2026-11-05)

The AP startup path was checked across `src/cpu.c`, `src/smp.c`,
`src/paging.c`, `src/tss.c`, `src/idt.c`, and the SMP trampoline.

MADT enumeration populates each discovered processor's APIC ID before
`smp_init()` sends INIT/SIPI. The AP then initializes its TSS and switches
to the shared kernel CR3 before `cpu_mark_online()` publishes its online
state. Those early operations use `cpu_current_index()` to select per-CPU
state.

Previously, `cpu_current_index()` matched an APIC ID only when the matching
CPU record was already marked online. During the AP bring-up interval, the AP
therefore fell through to index zero and could address the BSP's per-CPU
directory slot. The current AP code loads the same kernel CR3, so this did not
change the active page tables, but the lookup violated the per-CPU identity
contract and was unsafe as a basis for future AP-local initialization.

The lookup now resolves a configured APIC ID independently of the online
lifecycle flag. MADT-populated AP records are therefore addressable during
early AP initialization; `online` remains the separate indication that the
CPU has completed initialization and may participate in SMP operations.
If LAPIC identity is unavailable or does not match a configured record, the
legacy BSP fallback remains in place; this change does not add CPU hotplug,
x2APIC support, or a general unknown-CPU recovery path.

**Review boundary:** this is a CPU-identity correction only. It does not alter
AP startup sequencing, interrupt routing, TLB shootdown, scheduler policy,
GPU mappings, or xHCI code. CI/QEMU checks cannot replace a physical SMP
bring-up test on the Alder Lake laptop.
