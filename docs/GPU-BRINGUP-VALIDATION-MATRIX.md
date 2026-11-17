# GPU Bring-up Validation Matrix

Status: software-only validation plan; no hardware acceleration is enabled by this document.

## Current safety invariants

- The firmware-provided framebuffer remains the display output path.
- The CPU compositor remains the only operational graphics backend.
- PCI vendor/class matching may report a device as DETECTED; it does not prove initialization or operational rendering.
- Intel BAR0 MMIO access is confined to the validated mapped aperture. Register snapshots are read-only, except for the narrowly scoped, documented GT/render forcewake request and release writes required to power the diagnostic register domains. These writes do not program an engine, ring, GGTT entry, or display mode.
- GGTT entries are observed read-only. An empty PTE is not proof of ownership.
- `intel_gpu_memory_alloc()` allocates CPU-accessible system RAM. It does not make that memory GPU-addressable.
- `intel_gpu_memory_get_mapping_candidate()` must fail without changing caller outputs while GGTT ownership is unverified.
- No GGTT PTE writes, GPU TLB invalidations, ring programming, context submission, engine reset, or display-mode changes are permitted in the current bring-up state.
- No xHCI source changes are part of Graphics Engine v2.

## Software-only gates (CI/QEMU)

| Gate | Required evidence | Pass condition |
|---|---|---|
| PCI classification | `GFX-PCI-CLASSIFY-SELFTEST-OK` | Intel, AMD, NVIDIA, unknown vendor, non-display class, and invalid output cases behave as documented |
| PCI enumeration report | `GFX-PCI-DISCOVERY-OK` | Report is bounded; truncation is explicit; only display-class functions are recorded |
| BAR0 validation | `INTEL-BAR0-VALIDATION-SELFTEST-OK` | Type, size, alignment, address width, and boundary cases are rejected/accepted correctly |
| MMIO mapping | `KERNEL-MMIO-RANGE-SELFTEST-OK` | Range validation and transactional mapping tests pass |
| GGTT ownership gate | `INTEL-GGTT-OWNERSHIP-REPORT-SELFTEST-OK` | Missing evidence remains represented as blockers; mapping permission remains denied |
| GGTT PTE encoding | `INTEL-GGTT-PTE-ENCODE-SELFTEST-OK` | Valid aligned addresses encode; invalid addresses and null outputs fail without output mutation |
| Backing memory | `INTEL-GPU-MEM-2PAGE-SELFTEST-OK` | Allocation, CPU mapping, pin lifetime, read/write, free, stale-handle rejection, and unchanged GGTT snapshot pass |
| Desktop regression | `GFX-CPU-SELFTEST-OK` and GUI smoke markers | CPU rendering and input/desktop startup remain functional |

A marker is evidence that a test path ran, not proof of physical GPU execution. QEMU is a regression environment and cannot establish firmware GGTT ownership or real-device coherency.

## Physical-device evidence required before enabling GGTT mappings

The following evidence must be collected on each supported platform generation and firmware configuration:

1. **Firmware hand-off:** authoritative platform documentation and runtime evidence identifying which GGTT ranges are owned by firmware, display, and the OS.
2. **Display reservations:** exact scanout, cursor, compression, and other display-related allocations, including their lifetime and whether they may be relocated.
3. **PTE contract:** generation-specific PTE format, address-width rules, memory-type requirements, reserved bits, and supported page sizes, cross-checked against authoritative vendor documentation and a maintained driver implementation.
4. **Coherency:** documented CPU/GPU cache-coherency requirements for the chosen system-memory pages, including required cache flush/invalidate operations and ordering.
5. **Invalidation:** documented and tested GGTT/GPU TLB invalidation sequence, completion condition, and timeout/recovery behavior.
6. **Aperture bounds:** runtime validation that every proposed entry lies inside the implemented GGTT aperture and outside all reserved ranges.
7. **Failure recovery:** proof that partial setup can be rolled back without changing firmware scanout or leaving stale mappings.

All seven items must be reviewed before a mapping permission can be considered. A zero-valued PTE, a successful MMIO read, or a successful QEMU boot is insufficient evidence.

## Submission and rendering gates after GGTT ownership

GGTT ownership is necessary but not sufficient for command submission. Separate platform-specific gates are required for:

- engine discovery and supported engine selection;
- context image layout and context state initialization;
- ring or execlist submission protocol, including doorbells/CSB as applicable;
- command-stream validation and address-space isolation;
- hardware completion fences and interrupt/polling semantics;
- timeout, hang detection, reset policy, and resource retirement;
- a bounded first render workload with a CPU-rendered reference image and pixel comparison.

Until these gates pass, backend state must not advance to OPERATIONAL and automatic selection must retain the CPU fallback.

## Exit criteria for the current software phase

- CI build, Phase 26, and xHCI regression workflows are green on the same branch head.
- The complete changed-file diff has been reviewed twice.
- No xHCI source file is modified.
- Documentation and test markers do not overstate hardware capability.
- Physical-device validation remains explicitly pending.
