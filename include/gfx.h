#ifndef LIONOS_GFX_H
#define LIONOS_GFX_H

#include <stdint.h>

/*
 * Hardware-independent graphics surface.
 * Pixels use canonical 0xAARRGGBB ARGB8888 values in system RAM.
 * pitch_pixels is the distance between rows, in uint32_t pixels.
 * The caller owns the pixel storage and its lifetime.
 */
typedef struct {
    uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_pixels;
} gfx_surface_t;

typedef enum {
    GFX_BACKEND_NONE = 0,
    GFX_BACKEND_CPU = 1,
    GFX_BACKEND_GPU = 2
} gfx_backend_t;

typedef struct {
    gfx_surface_t target;
    gfx_backend_t backend;
    uint32_t initialized;
} gfx_context_t;

/* Validate and attach caller-owned ARGB8888 memory; no allocation occurs. */
int gfx_surface_validate(const gfx_surface_t *surface);
int gfx_context_init_cpu(gfx_context_t *context, const gfx_surface_t *target);

/* CPU rasterizer primitives. Coordinates outside the surface are ignored. */
int gfx_cpu_clear(gfx_context_t *context, uint32_t argb);
int gfx_cpu_put_pixel(gfx_context_t *context, uint32_t x, uint32_t y,
                      uint32_t argb);
int gfx_cpu_fill_rect(gfx_context_t *context, uint32_t x, uint32_t y,
                      uint32_t width, uint32_t height, uint32_t argb);

/* Deterministic in-memory test; does not touch the active framebuffer. */
int gfx_software_selftest(void);

#endif
