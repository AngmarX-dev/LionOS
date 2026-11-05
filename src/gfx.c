#include <stdint.h>
#include "gfx.h"

#define GFX_MAX_PIXEL_COUNT (UINT32_MAX / (uint32_t)sizeof(uint32_t))

int gfx_surface_validate(const gfx_surface_t *surface) {
    if (!surface || !surface->pixels || !surface->width || !surface->height)
        return -1;
    if (surface->pitch_pixels < surface->width)
        return -1;
    if (surface->pixel_format != GFX_PIXEL_FORMAT_ARGB8888 &&
        surface->pixel_format != GFX_PIXEL_FORMAT_XRGB8888)
        return -1;
    if (surface->height > GFX_MAX_PIXEL_COUNT / surface->pitch_pixels)
        return -1;
    return 0;
}

int gfx_context_init_cpu(gfx_context_t *context, const gfx_surface_t *target) {
    if (!context || gfx_surface_validate(target) != 0)
        return -1;

    context->target = *target;
    context->backend = GFX_BACKEND_CPU;
    context->initialized = 1u;
    return 0;
}

static uint32_t gfx_store_pixel(const gfx_surface_t *surface, uint32_t pixel) {
    return surface->pixel_format == GFX_PIXEL_FORMAT_XRGB8888
        ? (pixel & 0x00FFFFFFu) : pixel;
}

static int gfx_cpu_context_valid(const gfx_context_t *context) {
    return context && context->initialized == 1u &&
           context->backend == GFX_BACKEND_CPU &&
           gfx_surface_validate(&context->target) == 0;
}

int gfx_cpu_clear(gfx_context_t *context, uint32_t argb) {
    if (!gfx_cpu_context_valid(context))
        return -1;

    for (uint32_t y = 0u; y < context->target.height; ++y) {
        uint32_t *row = context->target.pixels +
                        y * context->target.pitch_pixels;
        for (uint32_t x = 0u; x < context->target.width; ++x)
            row[x] = gfx_store_pixel(&context->target, argb);
    }
    return 0;
}

int gfx_cpu_put_pixel(gfx_context_t *context, uint32_t x, uint32_t y,
                      uint32_t argb) {
    if (!gfx_cpu_context_valid(context))
        return -1;
    if (x >= context->target.width || y >= context->target.height)
        return 0;

    context->target.pixels[y * context->target.pitch_pixels + x] =
        gfx_store_pixel(&context->target, argb);
    return 0;
}

int gfx_cpu_fill_rect(gfx_context_t *context, uint32_t x, uint32_t y,
                      uint32_t width, uint32_t height, uint32_t argb) {
    if (!gfx_cpu_context_valid(context))
        return -1;
    if (!width || !height || x >= context->target.width ||
        y >= context->target.height)
        return 0;

    /* Subtraction-based clipping avoids overflow in x + width / y + height. */
    if (width > context->target.width - x)
        width = context->target.width - x;
    if (height > context->target.height - y)
        height = context->target.height - y;

    for (uint32_t row_index = 0u; row_index < height; ++row_index) {
        uint32_t *row = context->target.pixels +
            (y + row_index) * context->target.pitch_pixels + x;
        for (uint32_t column = 0u; column < width; ++column)
            row[column] = gfx_store_pixel(&context->target, argb);
    }
    return 0;
}

int gfx_software_selftest(void) {
    uint32_t storage[4u * 3u];
    for (uint32_t i = 0u; i < 12u; ++i)
        storage[i] = 0xDEADBEEFu;

    gfx_surface_t surface = {
        storage, 3u, 3u, 4u, GFX_PIXEL_FORMAT_ARGB8888
    };
    gfx_context_t context = {
        { 0, 0u, 0u, 0u, GFX_PIXEL_FORMAT_ARGB8888 },
        GFX_BACKEND_NONE, 0u
    };

    if (gfx_context_init_cpu(&context, &surface) != 0)
        return -1;
    if (gfx_cpu_clear(&context, 0xFF102030u) != 0)
        return -1;
    if (gfx_cpu_put_pixel(&context, 2u, 1u, 0xFFAABBCCu) != 0)
        return -1;
    if (gfx_cpu_put_pixel(&context, 3u, 1u, 0xFFFFFFFFu) != 0)
        return -1;
    if (gfx_cpu_fill_rect(&context, 1u, 2u, UINT32_MAX, 1u,
                          0xFF556677u) != 0)
        return -1;

    if (storage[0] != 0xFF102030u ||
        storage[1] != 0xFF102030u ||
        storage[4] != 0xFF102030u ||
        storage[2u + 1u * 4u] != 0xFFAABBCCu ||
        storage[1u + 2u * 4u] != 0xFF556677u ||
        storage[2u + 2u * 4u] != 0xFF556677u)
        return -1;

    /* Padding pixels must remain untouched by every raster operation. */
    if (storage[3] != 0xDEADBEEFu ||
        storage[7] != 0xDEADBEEFu ||
        storage[11] != 0xDEADBEEFu)
        return -1;

    surface.pitch_pixels = 2u;
    if (gfx_surface_validate(&surface) == 0)
        return -1;

    surface.pitch_pixels = 4u;
    surface.pixel_format = GFX_PIXEL_FORMAT_XRGB8888;
    if (gfx_context_init_cpu(&context, &surface) != 0)
        return -1;
    if (gfx_cpu_clear(&context, 0xFF102030u) != 0 ||
        storage[0] != 0x00102030u ||
        storage[1] != 0x00102030u ||
        storage[2] != 0x00102030u)
        return -1;
    return 0;
}
