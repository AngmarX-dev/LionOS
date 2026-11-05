#include <stdint.h>
#include "gfx.h"

#define GFX_MAX_PIXEL_COUNT (UINT32_MAX / (uint32_t)sizeof(uint32_t))

int gfx_surface_validate(const gfx_surface_t *surface) {
    if (!surface || !surface->pixels || !surface->width || !surface->height)
        return -1;
    if (((uintptr_t)surface->pixels & 3u) != 0u)
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


int gfx_cpu_blend_pixel(gfx_context_t *context, uint32_t x, uint32_t y,
                        uint32_t source_argb) {
    if (!gfx_cpu_context_valid(context))
        return -1;
    if (x >= context->target.width || y >= context->target.height)
        return 0;

    gfx_surface_t *surface = &context->target;
    uint32_t *destination = &surface->pixels[y * surface->pitch_pixels + x];
    const uint32_t source_alpha = source_argb >> 24;
    if (source_alpha == 0u)
        return 0;

    const uint32_t destination_pixel = *destination;
    const uint32_t destination_alpha =
        surface->pixel_format == GFX_PIXEL_FORMAT_XRGB8888
            ? 255u : (destination_pixel >> 24);
    const uint32_t inverse_source_alpha = 255u - source_alpha;
    const uint64_t output_alpha_numerator =
        (uint64_t)source_alpha * 255u +
        (uint64_t)destination_alpha * inverse_source_alpha;

    if (output_alpha_numerator == 0u) {
        *destination = 0u;
        return 0;
    }

    uint32_t output = 0u;
    for (uint32_t shift = 0u; shift <= 16u; shift += 8u) {
        const uint32_t source_channel = (source_argb >> shift) & 0xFFu;
        const uint32_t destination_channel =
            (destination_pixel >> shift) & 0xFFu;
        const uint64_t channel_numerator =
            (uint64_t)source_channel * source_alpha * 255u +
            (uint64_t)destination_channel * destination_alpha *
                inverse_source_alpha;
        const uint32_t channel = (uint32_t)(
            (channel_numerator + output_alpha_numerator / 2u) /
            output_alpha_numerator);
        output |= channel << shift;
    }

    if (surface->pixel_format == GFX_PIXEL_FORMAT_ARGB8888) {
        const uint32_t output_alpha = (uint32_t)(
            (output_alpha_numerator + 127u) / 255u);
        output |= output_alpha << 24;
    }
    *destination = output;
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

int gfx_cpu_draw_line(gfx_context_t *context, uint32_t x0, uint32_t y0,
                      uint32_t x1, uint32_t y1, uint32_t argb) {
    if (!gfx_cpu_context_valid(context))
        return -1;

    const gfx_surface_t *surface = &context->target;
    if (x0 >= surface->width || x1 >= surface->width ||
        y0 >= surface->height || y1 >= surface->height)
        return 0;

    /* Surface validation bounds coordinates well below INT64_MAX. */
    int64_t x = (int64_t)x0;
    int64_t y = (int64_t)y0;
    const int64_t end_x = (int64_t)x1;
    const int64_t end_y = (int64_t)y1;
    const int64_t dx = x0 <= x1 ? (int64_t)x1 - x0 : (int64_t)x0 - x1;
    const int64_t dy = y0 <= y1 ? (int64_t)y1 - y0 : (int64_t)y0 - y1;
    const int64_t step_x = x0 < x1 ? 1 : -1;
    const int64_t step_y = y0 < y1 ? 1 : -1;
    int64_t error = dx - dy;
    const uint32_t pixel = gfx_store_pixel(surface, argb);

    for (;;) {
        surface->pixels[(uint32_t)y * surface->pitch_pixels + (uint32_t)x] = pixel;
        if (x == end_x && y == end_y)
            break;

        const int64_t doubled_error = error * 2;
        if (doubled_error > -dy) {
            error -= dy;
            x += step_x;
        }
        if (doubled_error < dx) {
            error += dx;
            y += step_y;
        }
    }
    return 0;
}

static int64_t gfx_edge_value(int64_t ax, int64_t ay,
                               int64_t bx, int64_t by,
                               int64_t px, int64_t py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

int gfx_cpu_fill_triangle(gfx_context_t *context,
                          uint32_t x0, uint32_t y0,
                          uint32_t x1, uint32_t y1,
                          uint32_t x2, uint32_t y2, uint32_t argb) {
    if (!gfx_cpu_context_valid(context))
        return -1;

    const gfx_surface_t *surface = &context->target;
    if (x0 >= surface->width || x1 >= surface->width ||
        x2 >= surface->width || y0 >= surface->height ||
        y1 >= surface->height || y2 >= surface->height)
        return 0;

    /*
     * A valid surface contains at most UINT32_MAX / 4 pixels, so each
     * coordinate difference is bounded and the edge products fit int64_t.
     */
    const int64_t ax = (int64_t)x0;
    const int64_t ay = (int64_t)y0;
    const int64_t bx = (int64_t)x1;
    const int64_t by = (int64_t)y1;
    const int64_t cx = (int64_t)x2;
    const int64_t cy = (int64_t)y2;
    const int64_t area = gfx_edge_value(ax, ay, bx, by, cx, cy);
    if (area == 0)
        return 0;

    uint32_t min_x = x0 < x1 ? x0 : x1;
    uint32_t min_y = y0 < y1 ? y0 : y1;
    uint32_t max_x = x0 > x1 ? x0 : x1;
    uint32_t max_y = y0 > y1 ? y0 : y1;
    if (x2 < min_x) min_x = x2;
    if (y2 < min_y) min_y = y2;
    if (x2 > max_x) max_x = x2;
    if (y2 > max_y) max_y = y2;

    const uint32_t pixel = gfx_store_pixel(surface, argb);
    for (uint32_t y = min_y; y <= max_y; ++y) {
        uint32_t *row = surface->pixels + y * surface->pitch_pixels;
        for (uint32_t x = min_x; x <= max_x; ++x) {
            const int64_t px = (int64_t)x;
            const int64_t py = (int64_t)y;
            const int64_t w0 = gfx_edge_value(bx, by, cx, cy, px, py);
            const int64_t w1 = gfx_edge_value(cx, cy, ax, ay, px, py);
            const int64_t w2 = gfx_edge_value(ax, ay, bx, by, px, py);

            if ((area > 0 && w0 >= 0 && w1 >= 0 && w2 >= 0) ||
                (area < 0 && w0 <= 0 && w1 <= 0 && w2 <= 0))
                row[x] = pixel;
        }
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

    /* Straight-alpha source-over on opaque and transparent ARGB targets. */
    if (gfx_cpu_put_pixel(&context, 0u, 0u, 0xFF0000FFu) != 0 ||
        gfx_cpu_blend_pixel(&context, 0u, 0u, 0x80FF0000u) != 0 ||
        storage[0] != 0xFF80007Fu)
        return -1;
    if (gfx_cpu_put_pixel(&context, 1u, 0u, 0x00000000u) != 0 ||
        gfx_cpu_blend_pixel(&context, 1u, 0u, 0x80FF0000u) != 0 ||
        storage[1] != 0x80FF0000u)
        return -1;
    if (gfx_cpu_blend_pixel(&context, 2u, 0u, 0x00010203u) != 0 ||
        storage[2] != 0xFF102030u)
        return -1;

    if (gfx_cpu_fill_rect(&context, 1u, 2u, UINT32_MAX, 1u,
                          0xFF556677u) != 0)
        return -1;

    if (storage[0] != 0xFF80007Fu ||
        storage[1] != 0x80FF0000u ||
        storage[4] != 0xFF102030u ||
        storage[2u + 1u * 4u] != 0xFFAABBCCu ||
        storage[1u + 2u * 4u] != 0xFF556677u ||
        storage[2u + 2u * 4u] != 0xFF556677u)
        return -1;

    if (gfx_cpu_draw_line(&context, 0u, 0u, 2u, 2u,
                           0xFFABCDEFu) != 0 ||
        storage[0] != 0xFFABCDEFu ||
        storage[5] != 0xFFABCDEFu ||
        storage[10] != 0xFFABCDEFu)
        return -1;
    if (gfx_cpu_draw_line(&context, 0u, 0u, 3u, 2u,
                           0xFFFFFFFFu) != 0 ||
        storage[0] != 0xFFABCDEFu)
        return -1;

    if (gfx_cpu_fill_triangle(&context, 0u, 0u, 2u, 0u, 0u, 2u,
                              0xFF13579Bu) != 0 ||
        storage[0] != 0xFF13579Bu ||
        storage[1] != 0xFF13579Bu ||
        storage[2] != 0xFF13579Bu ||
        storage[4] != 0xFF13579Bu ||
        storage[5] != 0xFF13579Bu ||
        storage[8] != 0xFF13579Bu ||
        storage[6] != 0xFFAABBCCu)
        return -1;
    /* Reverse winding must cover the same triangle. */
    if (gfx_cpu_fill_triangle(&context, 0u, 2u, 2u, 0u, 0u, 0u,
                              0xFF2468ACu) != 0 ||
        storage[0] != 0xFF2468ACu ||
        storage[1] != 0xFF2468ACu ||
        storage[2] != 0xFF2468ACu ||
        storage[4] != 0xFF2468ACu ||
        storage[5] != 0xFF2468ACu ||
        storage[8] != 0xFF2468ACu)
        return -1;
    /* Out-of-bounds and degenerate triangles must not write pixels. */
    if (gfx_cpu_fill_triangle(&context, 0u, 0u, 2u, 0u, 3u, 2u,
                              0xFFFFFFFFu) != 0 ||
        gfx_cpu_fill_triangle(&context, 0u, 0u, 1u, 1u, 2u, 2u,
                              0xFFFFFFFFu) != 0 ||
        storage[0] != 0xFF2468ACu ||
        storage[6] != 0xFFAABBCCu)
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
        gfx_cpu_draw_line(&context, 0u, 2u, 2u, 2u, 0xFFABCDEFu) != 0 ||
        gfx_cpu_fill_triangle(&context, 0u, 2u, 2u, 0u, 0u, 0u,
                              0xFF13579Bu) != 0 ||
        storage[0] != 0x0013579Bu ||
        storage[1] != 0x0013579Bu ||
        storage[2] != 0x0013579Bu ||
        storage[4] != 0x0013579Bu ||
        storage[5] != 0x0013579Bu ||
        storage[8] != 0x0013579Bu ||
        storage[6] != 0x00102030u ||
        storage[9] != 0x00ABCDEFu ||
        storage[10] != 0x00ABCDEFu ||
        storage[3] != 0xDEADBEEFu ||
        storage[7] != 0xDEADBEEFu ||
        storage[11] != 0xDEADBEEFu)
        return -1;

    if (gfx_cpu_put_pixel(&context, 0u, 0u, 0x000000FFu) != 0 ||
        gfx_cpu_blend_pixel(&context, 0u, 0u, 0x80FF0000u) != 0 ||
        storage[0] != 0x0080007Fu ||
        gfx_cpu_blend_pixel(&context, 1u, 0u, 0x00FFFFFFu) != 0 ||
        storage[1] != 0x0013579Bu)
        return -1;
    return 0;
}
