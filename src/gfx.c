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

    /*
     * Both numerators are bounded: alpha <= 255*255 (65025), and each
     * channel numerator <= 255*65025 (16581375). uint32_t arithmetic is
     * therefore sufficient and avoids compiler runtime 64-bit division
     * helpers that are unavailable in the freestanding kernel.
     */
    const uint32_t output_alpha_numerator =
        source_alpha * 255u +
        destination_alpha * inverse_source_alpha;

    uint32_t output = 0u;
    for (uint32_t shift = 0u; shift <= 16u; shift += 8u) {
        const uint32_t source_channel = (source_argb >> shift) & 0xFFu;
        const uint32_t destination_channel =
            (destination_pixel >> shift) & 0xFFu;
        const uint32_t channel_numerator =
            source_channel * source_alpha * 255u +
            destination_channel * destination_alpha *
                inverse_source_alpha;
        const uint32_t channel =
            (channel_numerator + output_alpha_numerator / 2u) /
            output_alpha_numerator;
        output |= channel << shift;
    }

    if (surface->pixel_format == GFX_PIXEL_FORMAT_ARGB8888) {
        const uint32_t output_alpha =
            (output_alpha_numerator + 127u) / 255u;
        output |= output_alpha << 24;
    }
    *destination = output;
    return 0;
}

int gfx_cpu_blit(gfx_context_t *context, const gfx_surface_t *source,
                 uint32_t source_x, uint32_t source_y,
                 uint32_t destination_x, uint32_t destination_y,
                 uint32_t width, uint32_t height) {
    if (!gfx_cpu_context_valid(context) || gfx_surface_validate(source) != 0)
        return -1;
    if (width == 0u || height == 0u ||
        source_x >= source->width || source_y >= source->height ||
        destination_x >= context->target.width ||
        destination_y >= context->target.height)
        return 0;

    /* Reject aliasing conservatively; this keeps row traversal deterministic. */
    const uintptr_t source_begin = (uintptr_t)source->pixels;
    const uintptr_t destination_begin = (uintptr_t)context->target.pixels;
    const uintptr_t source_bytes =
        (uintptr_t)source->pitch_pixels * source->height * sizeof(uint32_t);
    const uintptr_t destination_bytes =
        (uintptr_t)context->target.pitch_pixels *
        context->target.height * sizeof(uint32_t);
    if (source_bytes > UINTPTR_MAX - source_begin ||
        destination_bytes > UINTPTR_MAX - destination_begin)
        return -1;
    const uintptr_t source_end = source_begin + source_bytes;
    const uintptr_t destination_end = destination_begin + destination_bytes;
    if (source_begin < destination_end && destination_begin < source_end)
        return -1;

    /* Subtraction-based clipping avoids overflow in coordinate additions. */
    if (width > source->width - source_x)
        width = source->width - source_x;
    if (width > context->target.width - destination_x)
        width = context->target.width - destination_x;
    if (height > source->height - source_y)
        height = source->height - source_y;
    if (height > context->target.height - destination_y)
        height = context->target.height - destination_y;

    for (uint32_t row = 0u; row < height; ++row) {
        const uint32_t *source_row = source->pixels +
            (source_y + row) * source->pitch_pixels + source_x;
        uint32_t *destination_row = context->target.pixels +
            (destination_y + row) * context->target.pitch_pixels +
            destination_x;
        for (uint32_t column = 0u; column < width; ++column) {
            uint32_t pixel = source_row[column];
            if (source->pixel_format == GFX_PIXEL_FORMAT_XRGB8888)
                pixel |= 0xFF000000u;
            destination_row[column] =
                gfx_store_pixel(&context->target, pixel);
        }
    }
    return 0;
}

int gfx_cpu_blit_scaled(gfx_context_t *context, const gfx_surface_t *source,
                        uint32_t source_x, uint32_t source_y,
                        uint32_t source_width, uint32_t source_height,
                        uint32_t destination_x, uint32_t destination_y,
                        uint32_t destination_width, uint32_t destination_height) {
    if (!gfx_cpu_context_valid(context) || gfx_surface_validate(source) != 0)
        return -1;
    if (source_x > source->width || source_y > source->height ||
        source_width > source->width - source_x ||
        source_height > source->height - source_y)
        return -1;
    if (source_width == 0u || source_height == 0u ||
        destination_width == 0u || destination_height == 0u ||
        destination_x >= context->target.width ||
        destination_y >= context->target.height)
        return 0;

    /* Reject aliasing conservatively; scaling has no in-place semantics. */
    const uintptr_t source_begin = (uintptr_t)source->pixels;
    const uintptr_t destination_begin = (uintptr_t)context->target.pixels;
    const uintptr_t source_bytes =
        (uintptr_t)source->pitch_pixels * source->height * sizeof(uint32_t);
    const uintptr_t destination_bytes =
        (uintptr_t)context->target.pitch_pixels *
        context->target.height * sizeof(uint32_t);
    if (source_bytes > UINTPTR_MAX - source_begin ||
        destination_bytes > UINTPTR_MAX - destination_begin)
        return -1;
    const uintptr_t source_end = source_begin + source_bytes;
    const uintptr_t destination_end = destination_begin + destination_bytes;
    if (source_begin < destination_end && destination_begin < source_end)
        return -1;

    uint32_t clipped_width = destination_width;
    uint32_t clipped_height = destination_height;
    if (clipped_width > context->target.width - destination_x)
        clipped_width = context->target.width - destination_x;
    if (clipped_height > context->target.height - destination_y)
        clipped_height = context->target.height - destination_y;

    /*
     * Incremental quotient/remainder stepping implements
     * floor(destination_index * source_size / destination_size) using only
     * 32-bit division. This avoids freestanding __udivdi3 dependencies.
     */
    const uint32_t x_step = source_width / destination_width;
    const uint32_t x_remainder = source_width % destination_width;
    const uint32_t y_step = source_height / destination_height;
    const uint32_t y_remainder = source_height % destination_height;
    uint32_t y_sample = 0u;
    uint32_t y_accumulator = 0u;

    for (uint32_t row = 0u; row < clipped_height; ++row) {
        const uint32_t *source_row = source->pixels +
            (source_y + y_sample) * source->pitch_pixels + source_x;
        uint32_t *destination_row = context->target.pixels +
            (destination_y + row) * context->target.pitch_pixels +
            destination_x;
        uint32_t x_sample = 0u;
        uint32_t x_accumulator = 0u;

        for (uint32_t column = 0u; column < clipped_width; ++column) {
            uint32_t pixel = source_row[x_sample];
            if (source->pixel_format == GFX_PIXEL_FORMAT_XRGB8888)
                pixel |= 0xFF000000u;
            destination_row[column] =
                gfx_store_pixel(&context->target, pixel);

            x_sample += x_step;
            if (x_remainder != 0u) {
                const uint32_t threshold = destination_width - x_remainder;
                if (x_accumulator >= threshold) {
                    x_accumulator -= threshold;
                    ++x_sample;
                } else {
                    x_accumulator += x_remainder;
                }
            }
        }

        y_sample += y_step;
        if (y_remainder != 0u) {
            const uint32_t threshold = destination_height - y_remainder;
            if (y_accumulator >= threshold) {
                y_accumulator -= threshold;
                ++y_sample;
            } else {
                y_accumulator += y_remainder;
            }
        }
    }
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

    /* Clipped ARGB -> XRGB blit; source and destination pitches differ. */
    uint32_t source_pixels[2u * 4u] = {
        0xFF010203u, 0x80445566u, 0xFF778899u, 0xDEADBEEFu,
        0xFF102030u, 0xFF405060u, 0xFF708090u, 0xDEADBEEFu
    };
    uint32_t destination_pixels[3u * 5u];
    for (uint32_t i = 0u; i < 15u; ++i)
        destination_pixels[i] = 0xCAFEBABEu;
    gfx_surface_t source_surface = {
        source_pixels, 3u, 2u, 4u, GFX_PIXEL_FORMAT_ARGB8888
    };
    gfx_surface_t destination_surface = {
        destination_pixels, 4u, 3u, 5u, GFX_PIXEL_FORMAT_XRGB8888
    };
    gfx_context_t blit_context = {
        { 0, 0u, 0u, 0u, GFX_PIXEL_FORMAT_ARGB8888 },
        GFX_BACKEND_NONE, 0u
    };
    if (gfx_context_init_cpu(&blit_context, &destination_surface) != 0 ||
        gfx_cpu_blit(&blit_context, &source_surface, 1u, 0u, 2u, 1u,
                     UINT32_MAX, 2u) != 0 ||
        destination_pixels[7] != 0x00445566u ||
        destination_pixels[8] != 0x00778899u ||
        destination_pixels[12] != 0x00405060u ||
        destination_pixels[13] != 0x00708090u ||
        destination_pixels[5] != 0xCAFEBABEu ||
        destination_pixels[9] != 0xCAFEBABEu ||
        destination_pixels[14] != 0xCAFEBABEu)
        return -1;

    /* XRGB -> ARGB must produce opaque pixels, not transparent ones. */
    source_surface.pixel_format = GFX_PIXEL_FORMAT_XRGB8888;
    source_pixels[0] = 0x00112233u;
    destination_surface.pixels = storage;
    destination_surface.width = 3u;
    destination_surface.height = 3u;
    destination_surface.pitch_pixels = 4u;
    destination_surface.pixel_format = GFX_PIXEL_FORMAT_ARGB8888;
    if (gfx_context_init_cpu(&blit_context, &destination_surface) != 0 ||
        gfx_cpu_blit(&blit_context, &source_surface, 0u, 0u, 1u, 1u,
                     1u, 1u) != 0 ||
        storage[5] != 0xFF112233u)
        return -1;

    /* Overlapping source/destination storage is rejected without writes. */
    source_surface.pixels = storage;
    source_surface.width = 3u;
    source_surface.height = 3u;
    source_surface.pitch_pixels = 4u;
    source_surface.pixel_format = GFX_PIXEL_FORMAT_ARGB8888;
    const uint32_t overlap_guard = storage[0];
    if (gfx_cpu_blit(&blit_context, &source_surface, 0u, 0u, 1u, 1u,
                     1u, 1u) != -1 ||
        storage[0] != overlap_guard)
        return -1;

    /* Nearest-neighbor scaling, destination clipping, and row padding. */
    {
        uint32_t scale_source_pixels[4] = {
            0xFF112233u, 0xFF445566u,
            0xFF778899u, 0xFFAABBCCu
        };
        uint32_t scale_destination_pixels[4u * 5u];
        for (uint32_t i = 0u; i < 20u; ++i)
            scale_destination_pixels[i] = 0xDEADBEEFu;
        gfx_surface_t scale_source = {
            scale_source_pixels, 2u, 2u, 2u, GFX_PIXEL_FORMAT_ARGB8888
        };
        gfx_surface_t scale_destination = {
            scale_destination_pixels, 4u, 4u, 5u,
            GFX_PIXEL_FORMAT_ARGB8888
        };
        gfx_context_t scale_context = {
            { 0, 0u, 0u, 0u, GFX_PIXEL_FORMAT_ARGB8888 },
            GFX_BACKEND_NONE, 0u
        };
        if (gfx_context_init_cpu(&scale_context, &scale_destination) != 0 ||
            gfx_cpu_blit_scaled(&scale_context, &scale_source,
                                0u, 0u, 2u, 2u, 0u, 0u, 4u, 4u) != 0)
            return -1;
        if (scale_destination_pixels[0] != 0xFF112233u ||
            scale_destination_pixels[1] != 0xFF112233u ||
            scale_destination_pixels[2] != 0xFF445566u ||
            scale_destination_pixels[3] != 0xFF445566u ||
            scale_destination_pixels[5] != 0xFF112233u ||
            scale_destination_pixels[7] != 0xFF445566u ||
            scale_destination_pixels[10] != 0xFF778899u ||
            scale_destination_pixels[12] != 0xFFAABBCCu ||
            scale_destination_pixels[4] != 0xDEADBEEFu ||
            scale_destination_pixels[9] != 0xDEADBEEFu ||
            scale_destination_pixels[14] != 0xDEADBEEFu ||
            scale_destination_pixels[19] != 0xDEADBEEFu)
            return -1;
        if (gfx_cpu_blit_scaled(&scale_context, &scale_source,
                                0u, 0u, 2u, 2u, 3u, 3u, 2u, 2u) != 0 ||
            scale_destination_pixels[18] != 0xFF112233u ||
            scale_destination_pixels[17] != 0xFFAABBCCu)
            return -1;
        if (gfx_cpu_blit_scaled(&scale_context, &scale_source,
                                1u, 1u, 2u, 1u, 0u, 0u, 2u, 2u) != -1)
            return -1;
        if (gfx_cpu_blit_scaled(&scale_context, &scale_source,
                                0u, 0u, UINT32_MAX, 1u,
                                0u, 0u, 2u, 2u) != -1)
            return -1;
    }

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
