# LionOS Graphics Engine v2

## Scope of this milestone

This branch separates desktop frame pacing from the process scheduler and introduces a reusable ARGB layer/compositing path for translucent windows. It is an incremental software compositor; it does not yet provide native Intel or NVIDIA GPU acceleration.

## Timing model

- PIT remains at 100 Hz and continues to drive the BSP process/scheduler clock.
- When the BSP local APIC timer can be calibrated against PIT channel 2, LionOS starts a separate 120 Hz periodic frame clock.
- The desktop loop waits for this frame clock instead of waiting for the PIT scheduler tick.
- USB mouse event-ring polling moves to the same BSP frame-clock interrupt when it is active. If APIC initialization or calibration is unavailable, USB mouse polling and desktop updates fall back to the existing PIT cadence.
- LAPIC frame ticks do not increment the kernel's PIT time counter. The APIC timer on application processors continues to drive their local scheduling path.

The 120 Hz value is a frame-update target, not a guarantee that every frame will finish within 8.33 ms. Physical refresh-rate discovery and tear-free VSync are not implemented yet.

## ARGB layer/compositor model

framebuffer_begin_layer() redirects the normal rectangle, rounded-rectangle, circle, text-fill, and RGBA-image drawing operations into an off-screen ARGB surface. Drawing uses straight-alpha source-over composition. framebuffer_end_layer() restores the previous drawing target and clip.

framebuffer_composite_layer() blends that ARGB surface into the desktop back buffer, respecting the active clip rectangle. During a window drag:

1. The desktop scene without the dragged window is captured once.
2. The dragged window is rasterized into an ARGB layer once.
3. Each frame restores only the old/new window union from the static scene.
4. The ARGB window layer is composited at its new position and the damaged rectangle is presented.

This avoids rerasterizing all window widgets, labels, rounded glass panels and icons at every mouse sample while preserving alpha against the current background.

## Self-test and CI

framebuffer_compositor_selftest() renders two semi-transparent colors into a 1x1 ARGB layer, composites it over a known background pixel, checks the resulting alpha/RGB values, and restores the original desktop-buffer pixel. GUI startup emits LIONOS:FRAMEBUFFER-ARGB-SELFTEST-OK only when this test passes. The CI GUI smoke test requires this marker.

The GitHub Actions QEMU jobs validate compilation, boot and the software compositor self-test. They do not prove physical display timing, physical USB mouse behavior, hardware VSync, or laptop-panel smoothness.

## Remaining work

1. Read the active display mode/refresh information and add a real presentation/vblank strategy where hardware exposes it.
2. Add frame-time, missed-deadline, copy-bandwidth and CPU-load telemetry.
3. Reduce memory bandwidth with tighter damage regions, row copies and buffering policy.
4. Add a safe native Intel display/render backend after platform and generation-specific hardware support is validated.
5. Consider NVIDIA support separately; the firmware framebuffer path remains the compatibility fallback.
6. Validate on real hardware, including window overlap, fast diagonal drag, transparency, edge clipping, multiple open windows, and fallback modes.
