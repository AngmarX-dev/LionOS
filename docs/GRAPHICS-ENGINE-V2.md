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
