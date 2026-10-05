#include <stdint.h>
#include "framebuffer.h"
#include "paging.h"
#include "heap.h"

#define FB_VIRTUAL_BASE 0xF4000000u
#define FB_MAX_MAPPED_SIZE 0x04000000u
#define MB2_TAG_FRAMEBUFFER 8u
#define MB2_TAG_END 0u
#define PAGE_SIZE 4096u

#define UI_TOPBAR 40u
#define UI_BOTTOMBAR 48u
#define UI_FONT_W 5u
#define UI_FONT_H 7u
#define UI_FONT_SCALE 2u
#define UI_CELL_W 12u
#define UI_CELL_H 16u
#define UI_LEFT 16u
#define UI_TOP 52u

struct mb2_tag { uint32_t type; uint32_t size; } __attribute__((packed));
struct mb2_fb_tag {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
    uint8_t red_field_position;
    uint8_t red_mask_size;
    uint8_t green_field_position;
    uint8_t green_mask_size;
    uint8_t blue_field_position;
    uint8_t blue_mask_size;
} __attribute__((packed));

static volatile uint8_t *fb;
static uint64_t fb_phys;
static uint32_t fb_pitch;
static uint32_t fb_width_value;
static uint32_t fb_height_value;
static uint8_t fb_bpp;
static uint8_t red_pos;
static uint8_t red_size;
static uint8_t green_pos;
static uint8_t green_size;
static uint8_t blue_pos;
static uint8_t blue_size;
static uint32_t enabled;
static struct mb2_fb_tag saved_fb_tag;
static uint32_t saved_fb_valid;
static uint32_t *desktop_buffer;
static uint32_t *wallpaper_cache;
static uint32_t wallpaper_cache_width;
static uint32_t wallpaper_cache_height;
static uint32_t wallpaper_cache_ready;
static uint32_t desktop_mode;
static uint8_t clip_enabled;
static uint32_t clip_x, clip_y, clip_w, clip_h;

/* GRUB/firmware selects the framebuffer mode before the kernel starts.
 * LionOS consumes the Multiboot2 framebuffer tag and has no runtime
 * display-mode switching interface. */
static uint32_t channel(uint8_t value, uint8_t size, uint8_t position) {
    if (!size) return 0u;
    uint32_t max = (1u << size) - 1u;
    uint32_t scaled = ((uint32_t)value * max + 127u) / 255u;
    return scaled << position;
}

static void write_packed_pixel(volatile uint8_t *dst, uint32_t packed) {
    if (fb_bpp == 32u) { *(volatile uint32_t *)dst = packed; return; }
    if (fb_bpp == 24u) { dst[0]=(uint8_t)(packed&0xFFu); dst[1]=(uint8_t)((packed>>8)&0xFFu); dst[2]=(uint8_t)((packed>>16)&0xFFu); return; }
    if (fb_bpp == 16u) { *(volatile uint16_t *)dst=(uint16_t)packed; return; }
}

static uint32_t fmt888;
static uint32_t pack_rgb(uint32_t rgb) {
    if(fmt888)return rgb&0x00FFFFFFu;
    uint8_t r = (uint8_t)(rgb >> 16);
    uint8_t g = (uint8_t)(rgb >> 8);
    uint8_t b = (uint8_t)rgb;
    return channel(r, red_size, red_pos) |
           channel(g, green_size, green_pos) |
           channel(b, blue_size, blue_pos);
}

static void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!enabled || x >= fb_width_value || y >= fb_height_value) return;
    uint32_t bytes=(fb_bpp+7u)/8u;
    write_packed_pixel(fb + y*fb_pitch + x*bytes, pack_rgb(color));
}

static void glyph(char c, uint8_t rows[UI_FONT_H]) {
    for (uint32_t i = 0; i < UI_FONT_H; ++i) rows[i] = 0;
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    switch (c) {
        case 'A': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x1F; rows[4]=0x11; rows[5]=0x11; rows[6]=0x11; break;
        case 'B': rows[0]=0x1E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x1E; rows[4]=0x11; rows[5]=0x11; rows[6]=0x1E; break;
        case 'C': rows[0]=0x0F; rows[1]=0x10; rows[2]=0x10; rows[3]=0x10; rows[4]=0x10; rows[5]=0x10; rows[6]=0x0F; break;
        case 'D': rows[0]=0x1E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x11; rows[4]=0x11; rows[5]=0x11; rows[6]=0x1E; break;
        case 'E': rows[0]=0x1F; rows[1]=0x10; rows[2]=0x10; rows[3]=0x1E; rows[4]=0x10; rows[5]=0x10; rows[6]=0x1F; break;
        case 'F': rows[0]=0x1F; rows[1]=0x10; rows[2]=0x10; rows[3]=0x1E; rows[4]=0x10; rows[5]=0x10; rows[6]=0x10; break;
        case 'G': rows[0]=0x0F; rows[1]=0x10; rows[2]=0x10; rows[3]=0x17; rows[4]=0x11; rows[5]=0x11; rows[6]=0x0F; break;
        case 'H': rows[0]=0x11; rows[1]=0x11; rows[2]=0x11; rows[3]=0x1F; rows[4]=0x11; rows[5]=0x11; rows[6]=0x11; break;
        case 'I': rows[0]=0x1F; rows[1]=0x04; rows[2]=0x04; rows[3]=0x04; rows[4]=0x04; rows[5]=0x04; rows[6]=0x1F; break;
        case 'J': rows[0]=0x1F; rows[1]=0x02; rows[2]=0x02; rows[3]=0x02; rows[4]=0x12; rows[5]=0x12; rows[6]=0x0C; break;
        case 'K': rows[0]=0x11; rows[1]=0x12; rows[2]=0x14; rows[3]=0x18; rows[4]=0x14; rows[5]=0x12; rows[6]=0x11; break;
        case 'L': rows[0]=0x10; rows[1]=0x10; rows[2]=0x10; rows[3]=0x10; rows[4]=0x10; rows[5]=0x10; rows[6]=0x1F; break;
        case 'M': rows[0]=0x11; rows[1]=0x1B; rows[2]=0x15; rows[3]=0x15; rows[4]=0x11; rows[5]=0x11; rows[6]=0x11; break;
        case 'N': rows[0]=0x11; rows[1]=0x19; rows[2]=0x15; rows[3]=0x13; rows[4]=0x11; rows[5]=0x11; rows[6]=0x11; break;
        case 'O': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x11; rows[4]=0x11; rows[5]=0x11; rows[6]=0x0E; break;
        case 'P': rows[0]=0x1E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x1E; rows[4]=0x10; rows[5]=0x10; rows[6]=0x10; break;
        case 'Q': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x11; rows[4]=0x15; rows[5]=0x12; rows[6]=0x0D; break;
        case 'R': rows[0]=0x1E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x1E; rows[4]=0x14; rows[5]=0x12; rows[6]=0x11; break;
        case 'S': rows[0]=0x0F; rows[1]=0x10; rows[2]=0x10; rows[3]=0x0E; rows[4]=0x01; rows[5]=0x01; rows[6]=0x1E; break;
        case 'T': rows[0]=0x1F; rows[1]=0x04; rows[2]=0x04; rows[3]=0x04; rows[4]=0x04; rows[5]=0x04; rows[6]=0x04; break;
        case 'U': rows[0]=0x11; rows[1]=0x11; rows[2]=0x11; rows[3]=0x11; rows[4]=0x11; rows[5]=0x11; rows[6]=0x0E; break;
        case 'V': rows[0]=0x11; rows[1]=0x11; rows[2]=0x11; rows[3]=0x11; rows[4]=0x0A; rows[5]=0x0A; rows[6]=0x04; break;
        case 'W': rows[0]=0x11; rows[1]=0x11; rows[2]=0x11; rows[3]=0x15; rows[4]=0x15; rows[5]=0x1B; rows[6]=0x11; break;
        case 'X': rows[0]=0x11; rows[1]=0x11; rows[2]=0x0A; rows[3]=0x04; rows[4]=0x0A; rows[5]=0x11; rows[6]=0x11; break;
        case 'Y': rows[0]=0x11; rows[1]=0x11; rows[2]=0x0A; rows[3]=0x04; rows[4]=0x04; rows[5]=0x04; rows[6]=0x04; break;
        case 'Z': rows[0]=0x1F; rows[1]=0x01; rows[2]=0x02; rows[3]=0x04; rows[4]=0x08; rows[5]=0x10; rows[6]=0x1F; break;
        case '0': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x13; rows[3]=0x15; rows[4]=0x19; rows[5]=0x11; rows[6]=0x0E; break;
        case '1': rows[0]=0x04; rows[1]=0x0C; rows[2]=0x04; rows[3]=0x04; rows[4]=0x04; rows[5]=0x04; rows[6]=0x0E; break;
        case '2': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x01; rows[3]=0x02; rows[4]=0x04; rows[5]=0x08; rows[6]=0x1F; break;
        case '3': rows[0]=0x1E; rows[1]=0x01; rows[2]=0x01; rows[3]=0x0E; rows[4]=0x01; rows[5]=0x01; rows[6]=0x1E; break;
        case '4': rows[0]=0x02; rows[1]=0x06; rows[2]=0x0A; rows[3]=0x12; rows[4]=0x1F; rows[5]=0x02; rows[6]=0x02; break;
        case '5': rows[0]=0x1F; rows[1]=0x10; rows[2]=0x10; rows[3]=0x1E; rows[4]=0x01; rows[5]=0x01; rows[6]=0x1E; break;
        case '6': rows[0]=0x0E; rows[1]=0x10; rows[2]=0x10; rows[3]=0x1E; rows[4]=0x11; rows[5]=0x11; rows[6]=0x0E; break;
        case '7': rows[0]=0x1F; rows[1]=0x01; rows[2]=0x02; rows[3]=0x04; rows[4]=0x08; rows[5]=0x08; rows[6]=0x08; break;
        case '8': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x0E; rows[4]=0x11; rows[5]=0x11; rows[6]=0x0E; break;
        case '9': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x0F; rows[4]=0x01; rows[5]=0x01; rows[6]=0x0E; break;
        case ':': rows[2]=0x04; rows[4]=0x04; break;
        case '.': rows[6]=0x04; break;
        case ',': rows[5]=0x04; rows[6]=0x08; break;
        case '-': rows[3]=0x1F; break;
        case '_': rows[6]=0x1F; break;
        case '/': rows[0]=0x01; rows[1]=0x02; rows[2]=0x04; rows[3]=0x08; rows[4]=0x10; break;
        case '>': rows[1]=0x10; rows[2]=0x08; rows[3]=0x04; rows[4]=0x08; rows[5]=0x10; break;
        case '<': rows[1]=0x01; rows[2]=0x02; rows[3]=0x04; rows[4]=0x02; rows[5]=0x01; break;
        case '!': rows[0]=0x04; rows[1]=0x04; rows[2]=0x04; rows[3]=0x04; rows[5]=0x04; break;
        case '?': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x01; rows[3]=0x02; rows[4]=0x04; rows[6]=0x04; break;
        case '|': rows[0]=0x04; rows[1]=0x04; rows[2]=0x04; rows[3]=0x04; rows[4]=0x04; rows[5]=0x04; rows[6]=0x04; break;
        case '=': rows[2]=0x1F; rows[4]=0x1F; break;
        case '(': rows[1]=0x02; rows[2]=0x04; rows[3]=0x04; rows[4]=0x04; rows[5]=0x04; rows[6]=0x02; break;
        case ')': rows[1]=0x08; rows[2]=0x04; rows[3]=0x04; rows[4]=0x04; rows[5]=0x04; rows[6]=0x08; break;
        case '[': rows[0]=0x0E; rows[1]=0x08; rows[2]=0x08; rows[3]=0x08; rows[4]=0x08; rows[5]=0x08; rows[6]=0x0E; break;
        case ']': rows[0]=0x0E; rows[1]=0x02; rows[2]=0x02; rows[3]=0x02; rows[4]=0x02; rows[5]=0x02; rows[6]=0x0E; break;
        case ' ': break;
        default: rows[0]=0x1F; rows[2]=0x15; rows[4]=0x15; rows[6]=0x1F; break;
    }
}

static void draw_char_at(char c, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg) {
    uint8_t rows[UI_FONT_H];
    glyph(c, rows);
    framebuffer_fill_rect(x, y, UI_CELL_W, UI_CELL_H, bg);
    for (uint32_t gy = 0; gy < UI_FONT_H; ++gy)
        for (uint32_t gx = 0; gx < UI_FONT_W; ++gx)
            if (rows[gy] & (1u << (UI_FONT_W - 1u - gx)))
                framebuffer_fill_rect(x + gx * UI_FONT_SCALE, y + gy * UI_FONT_SCALE,
                                      UI_FONT_SCALE, UI_FONT_SCALE, fg);
}

int framebuffer_prepare(uint32_t multiboot_info) {
    saved_fb_valid = 0u;
    uint8_t *cursor = (uint8_t *)(uintptr_t)(multiboot_info + 8u);
    for (;;) {
        struct mb2_tag *tag = (struct mb2_tag *)(uintptr_t)cursor;
        if (tag->type == MB2_TAG_END) break;
        if (tag->type == MB2_TAG_FRAMEBUFFER && tag->size >= sizeof(struct mb2_fb_tag)) {
            struct mb2_fb_tag *fb_tag = (struct mb2_fb_tag *)cursor;
            if (fb_tag->framebuffer_type != 1u ||
                (fb_tag->framebuffer_bpp != 16u && fb_tag->framebuffer_bpp != 24u && fb_tag->framebuffer_bpp != 32u))
                return -1;
            saved_fb_tag = *fb_tag;
            saved_fb_valid = 1u;
            return 0;
        }
        cursor += (tag->size + 7u) & ~7u;
    }
    return -1;
}

int framebuffer_init(uint32_t multiboot_info) {
    enabled = 0u;
    if (saved_fb_valid) {
        fb_phys = saved_fb_tag.framebuffer_addr;
        fb_pitch = saved_fb_tag.framebuffer_pitch;
        fb_width_value = saved_fb_tag.framebuffer_width;
        fb_height_value = saved_fb_tag.framebuffer_height;
        fb_bpp = saved_fb_tag.framebuffer_bpp;
        red_pos = saved_fb_tag.red_field_position; red_size = saved_fb_tag.red_mask_size;
        green_pos = saved_fb_tag.green_field_position; green_size = saved_fb_tag.green_mask_size;
        blue_pos = saved_fb_tag.blue_field_position; blue_size = saved_fb_tag.blue_mask_size;
        if (!fb_width_value || !fb_height_value || !fb_pitch) return -1;
        fmt888=(fb_bpp==32u&&red_size==8u&&green_size==8u&&blue_size==8u&&red_pos==16u&&green_pos==8u&&blue_pos==0u);
        goto map_framebuffer;
    }
    uint8_t *cursor = (uint8_t *)(uintptr_t)(multiboot_info + 8u);
    for (;;) {
        struct mb2_tag *tag = (struct mb2_tag *)(uintptr_t)cursor;
        if (tag->type == MB2_TAG_END) break;
        if (tag->type == MB2_TAG_FRAMEBUFFER && tag->size >= sizeof(struct mb2_fb_tag)) {
            struct mb2_fb_tag *fb_tag = (struct mb2_fb_tag *)cursor;
            if (fb_tag->framebuffer_type != 1u || (fb_tag->framebuffer_bpp != 16u && fb_tag->framebuffer_bpp != 24u && fb_tag->framebuffer_bpp != 32u)) return -1;
            fb_phys = fb_tag->framebuffer_addr;
            fb_pitch = fb_tag->framebuffer_pitch;
            fb_width_value = fb_tag->framebuffer_width;
            fb_height_value = fb_tag->framebuffer_height;
            fb_bpp = fb_tag->framebuffer_bpp;
            red_pos = fb_tag->red_field_position; red_size = fb_tag->red_mask_size;
            green_pos = fb_tag->green_field_position; green_size = fb_tag->green_mask_size;
            blue_pos = fb_tag->blue_field_position; blue_size = fb_tag->blue_mask_size;
map_framebuffer:
    fmt888=(fb_bpp==32u&&red_size==8u&&green_size==8u&&blue_size==8u&&red_pos==16u&&green_pos==8u&&blue_pos==0u);
            if (!fb_width_value || !fb_height_value || !fb_pitch) return -1;
            uint32_t offset = (uint32_t)(fb_phys & (PAGE_SIZE - 1u));
            uint64_t aligned = fb_phys & ~(uint64_t)(PAGE_SIZE - 1u);
            uint64_t bytes = (uint64_t)fb_pitch * fb_height_value + offset;
            uint32_t mapped = (uint32_t)((bytes + PAGE_SIZE - 1u) & ~(uint64_t)(PAGE_SIZE - 1u));
            if (!mapped || mapped > FB_MAX_MAPPED_SIZE) return -1;
            for (uint32_t off = 0; off < mapped; off += PAGE_SIZE)
                if (paging_map_kernel_page(FB_VIRTUAL_BASE + off, aligned + off, 0x3u) != 0) return -1;
            fb = (volatile uint8_t *)(uintptr_t)(FB_VIRTUAL_BASE + offset);
            enabled = 1u;
            framebuffer_console_clear();
            return 0;
        }
        cursor += (tag->size + 7u) & ~7u;
    }
    return -1;
}


static uint32_t boot_animation_frame;

static uint32_t boot_text_width(const char *s) {
    uint32_t n = 0u;
    if (!s) return 0u;
    while (s[n]) ++n;
    return n * UI_CELL_W;
}

static void boot_text_center(const char *s, uint32_t y, uint32_t fg, uint32_t bg) {
    uint32_t w = framebuffer_width();
    uint32_t tw = boot_text_width(s);
    uint32_t x = tw < w ? (w - tw) / 2u : 8u;
    while (s && *s && x + UI_CELL_W <= w) {
        draw_char_at(*s++, x, y, fg, bg);
        x += UI_CELL_W;
    }
}

void framebuffer_boot_splash(uint32_t progress, const char *status) {
    if (!enabled) return;
    if (progress > 100u) progress = 100u;
    uint32_t w = framebuffer_width(), h = framebuffer_height();
    if (!w || !h) return;

    ++boot_animation_frame;
    framebuffer_fill_rect(0u, 0u, w, h, 0x050A12u);
    framebuffer_fill_rect(0u, 0u, w, 4u, 0xF2C94Cu);

    uint32_t cx = w / 2u;
    uint32_t logo_y = h > 560u ? h / 2u - 150u : h / 2u - 120u;
    if (logo_y < 40u) logo_y = 40u;

    /* Compact gold LionOS mark built from the same geometry as the desktop theme. */
    uint32_t s = h > 560u ? 10u : 8u;
    uint32_t m = s * 2u + 1u;
    framebuffer_fill_rect(cx - m * 3u, logo_y, m * 2u, m, 0xF2C94Cu);
    framebuffer_fill_rect(cx + m, logo_y, m * 2u, m, 0xF2C94Cu);
    framebuffer_fill_rect(cx - m * 4u, logo_y + m, m * 8u, m * 5u, 0xF2C94Cu);
    framebuffer_fill_rect(cx - m * 3u, logo_y + m * 2u, m * 6u, m * 4u, 0x0B1422u);
    framebuffer_fill_rect(cx - m * 2u, logo_y + m * 2u, m, m, 0xF2C94Cu);
    framebuffer_fill_rect(cx + m, logo_y + m * 2u, m, m, 0xF2C94Cu);
    framebuffer_fill_rect(cx - m * 2u, logo_y + m * 5u, m * 4u, m * 2u, 0xF2C94Cu);
    framebuffer_fill_rect(cx - m, logo_y + m * 7u, m * 2u, m, 0xF2C94Cu);

    boot_text_center("LIONOS", logo_y + m * 9u, 0xF2C94Cu, 0x050A12u);
    boot_text_center("EXPERIMENTAL OPERATING SYSTEM", logo_y + m * 11u, 0x91A4BCu, 0x050A12u);

    uint32_t bar_w = w > 720u ? 560u : (w > 480u ? w - 120u : w - 64u);
    uint32_t bar_h = 10u;
    uint32_t bar_x = (w - bar_w) / 2u;
    uint32_t bar_y = h > 560u ? h - 120u : h - 90u;
    framebuffer_fill_rect(bar_x, bar_y, bar_w, bar_h, 0x13233Au);
    uint32_t fill_w = (bar_w * progress) / 100u;
    if (fill_w) framebuffer_fill_rect(bar_x, bar_y, fill_w, bar_h, 0xF2C94Cu);

    /* Moving highlight gives the loader a subtle animation without a backbuffer. */
    uint32_t marker = (boot_animation_frame * 6u) % (bar_w ? bar_w : 1u);
    if (marker + 12u < bar_w) framebuffer_fill_rect(bar_x + marker, bar_y, 12u, bar_h, 0xFFF1A8u);

    char percent[4] = {'0','0','0',0};
    uint32_t p = progress;
    percent[2] = (char)('0' + (p % 10u)); p /= 10u;
    percent[1] = (char)('0' + (p % 10u)); p /= 10u;
    percent[0] = (char)('0' + (p % 10u));
    boot_text_center(percent, bar_y + 20u, 0x91A4BCu, 0x050A12u);
    boot_text_center(status ? status : "Starting LionOS", bar_y + 42u, 0xF2F5FAu, 0x050A12u);
}

int framebuffer_available(void) { return enabled != 0u; }
uint32_t framebuffer_width(void) { return fb_width_value; }
uint32_t framebuffer_height(void) { return fb_height_value; }


int framebuffer_begin_desktop(void) {
    if (!enabled) return -1;
    if (desktop_mode) return 0;
    if (desktop_buffer) {
        desktop_mode = 1u;
        return 0;
    }
    uint64_t pixels = (uint64_t)fb_width_value * fb_height_value;
    if (pixels > 0xFFFFFFFFu / sizeof(uint32_t)) return -1;
    desktop_buffer = (uint32_t *)kmalloc((size_t)pixels * sizeof(uint32_t));
    if (!desktop_buffer) return -1;
    wallpaper_cache = 0;
    wallpaper_cache_width = fb_width_value;
    wallpaper_cache_height = fb_height_value;
    wallpaper_cache_ready = 0u;
    desktop_mode = 1u;
    return 0;
}

void framebuffer_end_desktop(void) {
    framebuffer_cursor_hide();
    clip_enabled = 0u;
    desktop_mode = 0u;
}

void framebuffer_set_clip(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!width || !height || x >= fb_width_value || y >= fb_height_value) {
        clip_enabled = 0u;
        return;
    }
    if (width > fb_width_value - x) width = fb_width_value - x;
    if (height > fb_height_value - y) height = fb_height_value - y;
    clip_x = x;
    clip_y = y;
    clip_w = width;
    clip_h = height;
    clip_enabled = 1u;
}

void framebuffer_clear_clip(void) {
    clip_enabled = 0u;
}

static void framebuffer_apply_clip(uint32_t *x, uint32_t *y, uint32_t *width, uint32_t *height) {
    if (!clip_enabled) return;
    if (*x < clip_x) {
        uint32_t delta = clip_x - *x;
        if (delta >= *width) { *width = 0u; return; }
        *x = clip_x;
        *width -= delta;
    }
    if (*y < clip_y) {
        uint32_t delta = clip_y - *y;
        if (delta >= *height) { *height = 0u; return; }
        *y = clip_y;
        *height -= delta;
    }
    uint32_t right = *x + *width;
    uint32_t clip_right = clip_x + clip_w;
    if (right > clip_right) *width = clip_right > *x ? clip_right - *x : 0u;
    uint32_t bottom = *y + *height;
    uint32_t clip_bottom = clip_y + clip_h;
    if (bottom > clip_bottom) *height = clip_bottom > *y ? clip_bottom - *y : 0u;
}

static const uint16_t cursor_arrow[] = {
    0x8000u,0xC000u,0xE000u,0xF000u,0xF800u,
    0xFC00u,0xFE00u,0xFF00u,0xFF80u,0xFFC0u,
    0xFFE0u,0xFFF0u,0xFFF8u,0xFFF0u,0xF3E0u,
    0xE1C0u,0xC080u,0x8000u
};
#define CURSOR_ARROW_W 16u
#define CURSOR_ARROW_H 18u
#define CURSOR_UNDER_W 17u
#define CURSOR_UNDER_H 19u
static uint32_t cursor_under[CURSOR_UNDER_W * CURSOR_UNDER_H];
static uint32_t cursor_under_w;
static uint32_t cursor_under_h;
static uint32_t cursor_under_x;
static uint32_t cursor_under_y;
static uint8_t cursor_visible;

int framebuffer_cursor_overlay_supported(void) {
    return enabled != 0u && desktop_mode != 0u && desktop_buffer != 0 && fb_bpp == 32u;
}

void framebuffer_cursor_hide(void) {
    if (!framebuffer_cursor_overlay_supported() || !cursor_visible) return;
    for (uint32_t y = 0u; y < cursor_under_h; ++y) {
        uint32_t sy = cursor_under_y + y;
        if (sy >= fb_height_value) break;
        volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)(fb + sy * fb_pitch + cursor_under_x * 4u);
        for (uint32_t x = 0u; x < cursor_under_w; ++x) {
            if (cursor_under_x + x >= fb_width_value) break;
            dst[x] = cursor_under[y * CURSOR_UNDER_W + x];
        }
    }
    cursor_visible = 0u;
}

void framebuffer_cursor_move(uint32_t x, uint32_t y) {
    if (!framebuffer_cursor_overlay_supported()) return;
    framebuffer_cursor_hide();
    if (x >= fb_width_value || y >= fb_height_value) return;

    cursor_under_x = x;
    cursor_under_y = y;
    cursor_under_w = CURSOR_UNDER_W;
    cursor_under_h = CURSOR_UNDER_H;
    if (cursor_under_x + cursor_under_w > fb_width_value)
        cursor_under_w = fb_width_value - cursor_under_x;
    if (cursor_under_y + cursor_under_h > fb_height_value)
        cursor_under_h = fb_height_value - cursor_under_y;

    for (uint32_t row = 0u; row < cursor_under_h; ++row) {
        uint32_t sy = cursor_under_y + row;
        volatile uint32_t *src = (volatile uint32_t *)(uintptr_t)(fb + sy * fb_pitch + cursor_under_x * 4u);
        for (uint32_t col = 0u; col < cursor_under_w; ++col)
            cursor_under[row * CURSOR_UNDER_W + col] = src[col];
    }

    uint32_t shadow = pack_rgb(0x050A12u);
    uint32_t white = pack_rgb(0xF2F5FAu);

    for (uint32_t row = 0u; row < CURSOR_ARROW_H; ++row) {
        uint16_t bits = cursor_arrow[row];
        for (uint32_t col = 0u; col < CURSOR_ARROW_W; ++col) {
            if (!(bits & (0x8000u >> col))) continue;
            uint32_t px0 = x + col + 1u;
            uint32_t py0 = y + row + 1u;
            if (px0 < fb_width_value && py0 < fb_height_value)
                *(volatile uint32_t *)(uintptr_t)(fb + py0 * fb_pitch + px0 * 4u) = shadow;
        }
    }
    for (uint32_t row = 1u; row + 1u < CURSOR_ARROW_H; ++row) {
        uint16_t bits = cursor_arrow[row];
        for (uint32_t col = 1u; col + 1u < CURSOR_ARROW_W; ++col) {
            if (!(bits & (0x8000u >> col))) continue;
            uint32_t px0 = x + col;
            uint32_t py0 = y + row;
            if (px0 < fb_width_value && py0 < fb_height_value)
                *(volatile uint32_t *)(uintptr_t)(fb + py0 * fb_pitch + px0 * 4u) = white;
        }
    }
    cursor_visible = 1u;
}

void framebuffer_present_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!enabled || !desktop_mode || !desktop_buffer || !width || !height) return;
    if (x >= fb_width_value || y >= fb_height_value) return;
    if (width > fb_width_value - x) width = fb_width_value - x;
    if (height > fb_height_value - y) height = fb_height_value - y;
    uint32_t bytes=(fb_bpp+7u)/8u;
    if(fb_bpp==32u){
        size_t row_bytes=(size_t)width*4u;
        for(uint32_t row=0u;row<height;++row){
            void *dst=(void *)(uintptr_t)(fb+(y+row)*fb_pitch+x*4u);
            const void *src=(const void *)(desktop_buffer+(y+row)*fb_width_value+x);
            __builtin_memcpy(dst,src,row_bytes);
        }
        return;
    }
    for(uint32_t row=0u;row<height;++row){
        volatile uint8_t *dst=fb+(y+row)*fb_pitch+x*bytes;
        const uint32_t *src=desktop_buffer+(y+row)*fb_width_value+x;
        for(uint32_t col=0u;col<width;++col)
            write_packed_pixel(dst+col*bytes,src[col]);
    }
}

void framebuffer_present(void) {
    framebuffer_present_rect(0u,0u,fb_width_value,fb_height_value);
}

void framebuffer_copy_to_buffer(uint32_t *dst) {
    if (!dst || !desktop_mode || !desktop_buffer) return;
    __builtin_memcpy(dst, desktop_buffer,
                     (size_t)fb_width_value * fb_height_value * sizeof(uint32_t));
}

void framebuffer_copy_rect_to_buffer(uint32_t *dst, uint32_t dst_stride,
                                     uint32_t x, uint32_t y,
                                     uint32_t width, uint32_t height) {
    if (!dst || !desktop_mode || !desktop_buffer || !dst_stride ||
        x >= fb_width_value || y >= fb_height_value || !width || !height) return;
    if (width > fb_width_value - x) width = fb_width_value - x;
    if (height > fb_height_value - y) height = fb_height_value - y;
    for (uint32_t row=0u; row<height; ++row) {
        __builtin_memcpy(dst + row*dst_stride,
                         desktop_buffer + (y+row)*fb_width_value + x,
                         (size_t)width*sizeof(uint32_t));
    }
}

void framebuffer_copy_rect_from_buffer(const uint32_t *src, uint32_t src_stride,
                                       uint32_t x, uint32_t y,
                                       uint32_t width, uint32_t height) {
    if (!src || !desktop_mode || !desktop_buffer || !src_stride ||
        x >= fb_width_value || y >= fb_height_value || !width || !height) return;
    if (width > fb_width_value - x) width = fb_width_value - x;
    if (height > fb_height_value - y) height = fb_height_value - y;
    for (uint32_t row=0u; row<height; ++row) {
        __builtin_memcpy(desktop_buffer + (y+row)*fb_width_value + x,
                         src + row*src_stride,
                         (size_t)width*sizeof(uint32_t));
    }
}

void framebuffer_blit_rgba32(const uint32_t *pixels, uint32_t width, uint32_t height, uint32_t x, uint32_t y, uint32_t size) {
    if (!enabled || !desktop_mode || !desktop_buffer || !pixels || !width || !height || !size) return;
    uint32_t start_x = 0u, end_x = size, start_y = 0u, end_y = size;
    if (clip_enabled) {
        uint32_t clip_right = clip_x + clip_w, clip_bottom = clip_y + clip_h;
        if (x < clip_x) start_x = clip_x - x;
        if (y < clip_y) start_y = clip_y - y;
        if (x + end_x > clip_right) end_x = clip_right > x ? clip_right - x : 0u;
        if (y + end_y > clip_bottom) end_y = clip_bottom > y ? clip_bottom - y : 0u;
    }
    if (x + start_x >= fb_width_value || y + start_y >= fb_height_value ||
        end_x <= start_x || end_y <= start_y) return;
    if (end_x > fb_width_value - x) end_x = fb_width_value - x;
    if (end_y > fb_height_value - y) end_y = fb_height_value - y;
    for (uint32_t dy = start_y; dy < end_y; ++dy) {
        uint32_t sy = (dy * height) / size;
        for (uint32_t dx = start_x; dx < end_x; ++dx) {
            uint32_t sx = (dx * width) / size;
            uint32_t src = pixels[sy * width + sx];
            uint32_t alpha = src >> 24;
            if (alpha == 0u) continue;
            uint32_t rgb = src & 0x00FFFFFFu;
            if (alpha >= 255u) {
                desktop_buffer[(y + dy) * fb_width_value + (x + dx)] = pack_rgb(rgb);
                continue;
            }
            uint32_t dst = desktop_buffer[(y + dy) * fb_width_value + (x + dx)];
            uint32_t sr = (rgb >> 16) & 0xFFu, sg = (rgb >> 8) & 0xFFu, sb = rgb & 0xFFu;
            uint32_t dr = (dst >> red_pos) & ((1u << red_size) - 1u);
            uint32_t dg = (dst >> green_pos) & ((1u << green_size) - 1u);
            uint32_t db = (dst >> blue_pos) & ((1u << blue_size) - 1u);
            dr = (dr * 255u) / ((1u << red_size) - 1u);
            dg = (dg * 255u) / ((1u << green_size) - 1u);
            db = (db * 255u) / ((1u << blue_size) - 1u);
            uint32_t rr = (sr * alpha + dr * (255u - alpha)) / 255u;
            uint32_t rg = (sg * alpha + dg * (255u - alpha)) / 255u;
            uint32_t rb = (sb * alpha + db * (255u - alpha)) / 255u;
            desktop_buffer[(y + dy) * fb_width_value + (x + dx)] = pack_rgb((rr << 16) | (rg << 8) | rb);
        }
    }
}

void framebuffer_blit_rgb565_cover(const uint16_t *pixels, uint32_t width, uint32_t height) {
    if (!enabled || !desktop_mode || !desktop_buffer || !pixels || !width || !height) return;
    if (!wallpaper_cache || wallpaper_cache_width != fb_width_value || wallpaper_cache_height != fb_height_value) {
        uint64_t pixels_count=(uint64_t)fb_width_value*fb_height_value;
        if (pixels_count > 0xFFFFFFFFu/sizeof(uint32_t)) return;
        if (wallpaper_cache) kfree(wallpaper_cache);
        wallpaper_cache=(uint32_t*)kmalloc((size_t)pixels_count*sizeof(uint32_t));
        if (!wallpaper_cache) { wallpaper_cache_ready=0u; return; }
        wallpaper_cache_width=fb_width_value; wallpaper_cache_height=fb_height_value; wallpaper_cache_ready=0u;
    }
    uint32_t sw=fb_width_value, sh=fb_height_value;
    if (!wallpaper_cache_ready) {
        for(uint64_t i=0u;i<(uint64_t)sw*sh;++i) wallpaper_cache[i]=pack_rgb(0u);
        uint64_t lhs=(uint64_t)sw*height, rhs=(uint64_t)sh*width;
        uint32_t out_w,out_h;
        if(lhs>=rhs){out_w=sw;out_h=(uint32_t)(lhs/width);}
        else{out_h=sh;out_w=(uint32_t)(rhs/height);}
        uint32_t crop_x=out_w>sw?(out_w-sw)/2u:0u, crop_y=out_h>sh?(out_h-sh)/2u:0u;
        for(uint32_t y=0u;y<sh;++y){
            uint32_t sy=((uint64_t)(y+crop_y)*height)/out_h;
            uint32_t *dst=wallpaper_cache+y*sw;
            for(uint32_t x=0u;x<sw;++x){
                uint32_t sx=((uint64_t)(x+crop_x)*width)/out_w;
                uint16_t p=pixels[sy*width+sx];
                uint32_t r=((p>>11)&31u)*255u/31u, g=((p>>5)&63u)*255u/63u, b=(p&31u)*255u/31u;
                dst[x]=pack_rgb((r<<16)|(g<<8)|b);
            }
        }
        wallpaper_cache_ready=1u;
    }
    if (clip_enabled)
        framebuffer_restore_wallpaper_rect(clip_x,clip_y,clip_w,clip_h);
    else
        framebuffer_restore_wallpaper();
}
void framebuffer_restore_wallpaper_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!enabled || !desktop_mode || !desktop_buffer || !wallpaper_cache || !wallpaper_cache_ready || !width || !height) return;
    if (x >= fb_width_value || y >= fb_height_value) return;
    if (width > fb_width_value - x) width = fb_width_value - x;
    if (height > fb_height_value - y) height = fb_height_value - y;
    for (uint32_t row = 0u; row < height; ++row) {
        uint32_t *dst = desktop_buffer + (y + row) * fb_width_value + x;
        const uint32_t *src = wallpaper_cache + (y + row) * fb_width_value + x;
        __builtin_memcpy(dst, src, (size_t)width * sizeof(uint32_t));
    }
}

void framebuffer_restore_wallpaper(void) {
    if (!enabled || !desktop_mode || !desktop_buffer || !wallpaper_cache || !wallpaper_cache_ready) return;
    uint32_t count=fb_width_value*fb_height_value;
    __builtin_memcpy(desktop_buffer,wallpaper_cache,(size_t)count*sizeof(uint32_t));
}

void framebuffer_clear(uint32_t color) {
    framebuffer_fill_rect(0u, 0u, fb_width_value, fb_height_value, color);
}

void framebuffer_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color) {
    if (!enabled) return;
    if (x >= fb_width_value || y >= fb_height_value || !width || !height) return;
    if (width > fb_width_value - x) width = fb_width_value - x;
    if (height > fb_height_value - y) height = fb_height_value - y;
    framebuffer_apply_clip(&x, &y, &width, &height);
    if (!width || !height) return;
    uint32_t packed = pack_rgb(color);
    for (uint32_t yy = y; yy < y + height; ++yy) {
        if(desktop_mode){
            uint32_t *line=desktop_buffer+yy*fb_width_value+x;
            for(uint32_t xx=0u;xx<width;++xx)line[xx]=packed;
        }else{
            uint32_t bytes=(fb_bpp+7u)/8u;
            volatile uint8_t *line=fb+yy*fb_pitch+x*bytes;
            for(uint32_t xx=0u;xx<width;++xx)write_packed_pixel(line+xx*bytes,packed);
        }
    }
}

static uint32_t desktop_unpack_rgb(uint32_t packed){
    uint32_t rmask=red_size?((1u<<red_size)-1u):0u;
    uint32_t gmask=green_size?((1u<<green_size)-1u):0u;
    uint32_t bmask=blue_size?((1u<<blue_size)-1u):0u;
    uint32_t r=red_size?((packed>>red_pos)&rmask):0u;
    uint32_t g=green_size?((packed>>green_pos)&gmask):0u;
    uint32_t b=blue_size?((packed>>blue_pos)&bmask):0u;
    if(rmask)r=(r*255u+rmask/2u)/rmask;
    if(gmask)g=(g*255u+gmask/2u)/gmask;
    if(bmask)b=(b*255u+bmask/2u)/bmask;
    return (r<<16)|(g<<8)|b;
}

static uint32_t desktop_blend_rgb(uint32_t dst,uint32_t src,uint32_t alpha){
    if(alpha>=255u)return src&0x00FFFFFFu;
    if(alpha==0u)return dst&0x00FFFFFFu;
    uint32_t dr=(dst>>16)&255u,dg=(dst>>8)&255u,db=dst&255u;
    uint32_t sr=(src>>16)&255u,sg=(src>>8)&255u,sb=src&255u;
    uint32_t r=(sr*alpha+dr*(255u-alpha))/255u;
    uint32_t g=(sg*alpha+dg*(255u-alpha))/255u;
    uint32_t b=(sb*alpha+db*(255u-alpha))/255u;
    return (r<<16)|(g<<8)|b;
}

static inline uint32_t blend888(uint32_t dst,uint32_t src,uint32_t a){uint32_t rb=(((src&0x00FF00FFu)*a+(dst&0x00FF00FFu)*(256u-a))>>8)&0x00FF00FFu;uint32_t g=(((src&0x0000FF00u)*a+(dst&0x0000FF00u)*(256u-a))>>8)&0x0000FF00u;return rb|g;}
static void blend_span(uint32_t*p,uint32_t n,uint32_t color,uint32_t alpha){if(fmt888){uint32_t a=alpha+(alpha>>7),s=color&0x00FFFFFFu;for(uint32_t i=0;i<n;++i)p[i]=blend888(p[i],s,a);return;}for(uint32_t i=0;i<n;++i)p[i]=pack_rgb(desktop_blend_rgb(desktop_unpack_rgb(p[i]),color,alpha));}
static uint32_t isqrt_u32(uint32_t v){uint32_t r=0u;while((r+1u)*(r+1u)<=v)++r;return r;}
void framebuffer_blend_rect(uint32_t x,uint32_t y,uint32_t width,uint32_t height,uint32_t color,uint8_t alpha){if(!enabled||!desktop_mode||!desktop_buffer||!width||!height||!alpha)return;if(x>=fb_width_value||y>=fb_height_value)return;if(width>fb_width_value-x)width=fb_width_value-x;if(height>fb_height_value-y)height=fb_height_value-y;uint32_t x0=x,y0=y,x1=x+width,y1=y+height;if(clip_enabled){if(x0<clip_x)x0=clip_x;if(y0<clip_y)y0=clip_y;uint32_t cr=clip_x+clip_w,cb=clip_y+clip_h;if(x1>cr)x1=cr;if(y1>cb)y1=cb;}if(x1<=x0||y1<=y0)return;for(uint32_t yy=y0;yy<y1;++yy)blend_span(desktop_buffer+yy*fb_width_value+x0,x1-x0,color,alpha);}
void framebuffer_blend_round_rect(uint32_t x,uint32_t y,uint32_t width,uint32_t height,uint32_t radius,uint32_t color,uint8_t alpha){if(!enabled||!desktop_mode||!desktop_buffer||!width||!height||!alpha)return;if(x>=fb_width_value||y>=fb_height_value)return;if(width>fb_width_value-x)width=fb_width_value-x;if(height>fb_height_value-y)height=fb_height_value-y;if(radius*2u>width)radius=width/2u;if(radius*2u>height)radius=height/2u;uint32_t x0=x,y0=y,x1=x+width,y1=y+height;if(clip_enabled){if(x0<clip_x)x0=clip_x;if(y0<clip_y)y0=clip_y;uint32_t cr=clip_x+clip_w,cb=clip_y+clip_h;if(x1>cr)x1=cr;if(y1>cb)y1=cb;}if(x1<=x0||y1<=y0)return;int32_t r=(int32_t)radius,left=(int32_t)x,top=(int32_t)y,right=(int32_t)(x+width-1u),bottom=(int32_t)(y+height-1u);for(uint32_t yy=y0;yy<y1;++yy){int32_t py=(int32_t)yy,dy=0,inset=0;if(py<top+r)dy=top+r-py;else if(py>bottom-r)dy=py-(bottom-r);if(dy>0)inset=r-(int32_t)isqrt_u32((uint32_t)(r*r-dy*dy));uint32_t sx=(uint32_t)(left+inset),ex=(uint32_t)(right-inset)+1u;if(sx<x0)sx=x0;if(ex>x1)ex=x1;if(ex>sx)blend_span(desktop_buffer+yy*fb_width_value+sx,ex-sx,color,alpha);}}
void framebuffer_blend_circle(uint32_t cx,uint32_t cy,uint32_t radius,uint32_t color,uint8_t alpha){if(!enabled||!desktop_mode||!desktop_buffer||!radius||!alpha)return;int32_t r=(int32_t)radius,y0=(int32_t)cy-r,y1=(int32_t)cy+r+1,cl=0,cr=(int32_t)fb_width_value,ct=0,cb=(int32_t)fb_height_value;if(clip_enabled){cl=(int32_t)clip_x;cr=(int32_t)(clip_x+clip_w);ct=(int32_t)clip_y;cb=(int32_t)(clip_y+clip_h);}if(y0<ct)y0=ct;if(y1>cb)y1=cb;for(int32_t yy=y0;yy<y1;++yy){int32_t dy=yy-(int32_t)cy,d2=r*r-dy*dy;if(d2<0)continue;int32_t half=(int32_t)isqrt_u32((uint32_t)d2),sx=(int32_t)cx-half,ex=(int32_t)cx+half+1;if(sx<cl)sx=cl;if(ex>cr)ex=cr;if(ex>sx)blend_span(desktop_buffer+(uint32_t)yy*fb_width_value+(uint32_t)sx,(uint32_t)(ex-sx),color,alpha);}}

void framebuffer_console_clear(void) {
    if (!enabled) return;
    framebuffer_clear(0x07111Fu);
    framebuffer_fill_rect(0u, 0u, fb_width_value, UI_TOPBAR, 0x102A43u);
    framebuffer_fill_rect(0u, UI_TOPBAR, fb_width_value, 4u, 0xF2C14Eu);
    framebuffer_fill_rect(0u, fb_height_value - UI_BOTTOMBAR, fb_width_value, UI_BOTTOMBAR, 0x0B1728u);
    uint32_t dock_y = fb_height_value - UI_BOTTOMBAR + 10u;
    framebuffer_fill_rect(18u, dock_y, 42u, 28u, 0x173A5Eu);
    framebuffer_fill_rect(70u, dock_y, 42u, 28u, 0x214F78u);
    framebuffer_fill_rect(122u, dock_y, 42u, 28u, 0x173A5Eu);
}

void framebuffer_console_putc(char c, uint32_t row, uint32_t col, uint32_t fg, uint32_t bg) {
    if (!enabled) return;
    uint32_t rows = (fb_height_value > UI_TOP + UI_BOTTOMBAR) ? (fb_height_value - UI_TOP - UI_BOTTOMBAR) / UI_CELL_H : 0u;
    uint32_t cols = (fb_width_value > UI_LEFT) ? (fb_width_value - UI_LEFT) / UI_CELL_W : 0u;
    if (row >= rows || col >= cols) return;
    draw_char_at(c, UI_LEFT + col * UI_CELL_W, UI_TOP + row * UI_CELL_H, fg, bg);
}
