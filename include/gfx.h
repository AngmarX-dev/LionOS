#ifndef LIONOS_GFX_H
#define LIONOS_GFX_H

#include <stdint.h>

/*
 * Hardware-independent graphics surface.
 * Pixels use either 0xAARRGGBB ARGB8888 or 0x00RRGGBB XRGB8888.
 * XRGB writes discard the high byte. pitch_pixels is measured in uint32_t pixels.
 * Storage must remain valid for the context lifetime; ownership is not transferred.
 */
typedef enum {
    GFX_PIXEL_FORMAT_ARGB8888 = 1,
    GFX_PIXEL_FORMAT_XRGB8888 = 2
} gfx_pixel_format_t;

typedef struct {
    uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_pixels;
    gfx_pixel_format_t pixel_format;
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

/* Validate and attach caller-owned pixel memory; no allocation occurs. */
int gfx_surface_validate(const gfx_surface_t *surface);
int gfx_context_init_cpu(gfx_context_t *context, const gfx_surface_t *target);

/* CPU rasterizer primitives. Coordinates outside the surface are ignored. */
int gfx_cpu_clear(gfx_context_t *context, uint32_t argb);
int gfx_cpu_put_pixel(gfx_context_t *context, uint32_t x, uint32_t y,
                      uint32_t argb);
/* Source-over alpha compositing using straight-alpha ARGB input. */
int gfx_cpu_blend_pixel(gfx_context_t *context, uint32_t x, uint32_t y,
                        uint32_t source_argb);
int gfx_cpu_fill_rect(gfx_context_t *context, uint32_t x, uint32_t y,
                      uint32_t width, uint32_t height, uint32_t argb);
/* Draws a line when both endpoints are inside the target surface. */
int gfx_cpu_draw_line(gfx_context_t *context, uint32_t x0, uint32_t y0,
                      uint32_t x1, uint32_t y1, uint32_t argb);
/* Fills a triangle; all three vertices must be inside the target surface. */
int gfx_cpu_fill_triangle(gfx_context_t *context,
                          uint32_t x0, uint32_t y0,
                          uint32_t x1, uint32_t y1,
                          uint32_t x2, uint32_t y2, uint32_t argb);

/* Deterministic in-memory test; does not touch the active framebuffer. */
int gfx_software_selftest(void);

#endif
