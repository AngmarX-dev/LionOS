#include <stdint.h>
#include "framebuffer.h"
#include "paging.h"
#include "heap.h"
#include "io.h"

#define FB_VIRTUAL_BASE 0xF0000000u
#define FB_MAX_MAPPED_SIZE 0x01000000u
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
static uint32_t fb_phys;
static uint32_t fb_pitch;
static uint32_t fb_width_value;
static uint32_t fb_height_value;
static uint8_t red_pos;
static uint8_t red_size;
static uint8_t green_pos;
static uint8_t green_size;
static uint8_t blue_pos;
static uint8_t blue_size;
static uint32_t enabled;
static uint32_t *desktop_buffer;
static uint32_t *wallpaper_cache;
static uint32_t wallpaper_cache_width;
static uint32_t wallpaper_cache_height;
static uint32_t wallpaper_cache_ready;
static uint32_t desktop_mode;

/* QEMU/Bochs VBE DISPI interface. The driver is probed by writing a mode and
 * reading it back, so real hardware that does not expose this interface is
 * left untouched. */
#define VBE_INDEX_PORT 0x01CEu
#define VBE_DATA_PORT  0x01CFu
#define VBE_XRES 1u
#define VBE_YRES 2u
#define VBE_BPP 3u
#define VBE_ENABLE 4u
#define VBE_ENABLED 0x01u
#define VBE_LFB_ENABLED 0x40u

static void vbe_write(uint16_t index, uint16_t value) { outw(VBE_INDEX_PORT, index); outw(VBE_DATA_PORT, value); }
static uint16_t vbe_read(uint16_t index) { outw(VBE_INDEX_PORT, index); return inw(VBE_DATA_PORT); }

static int vbe_set_mode(uint32_t width, uint32_t height) {
    if (width < 640u || height < 480u || width > 4096u || height > 2160u) return -1;
    vbe_write(VBE_ENABLE, 0u);
    vbe_write(VBE_XRES, (uint16_t)width);
    vbe_write(VBE_YRES, (uint16_t)height);
    vbe_write(VBE_BPP, 32u);
    vbe_write(VBE_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED);
    if (vbe_read(VBE_XRES) != (uint16_t)width || vbe_read(VBE_YRES) != (uint16_t)height) return -1;
    return 0;
}

static uint32_t channel(uint8_t value, uint8_t size, uint8_t position) {
    if (!size) return 0u;
    uint32_t max = (1u << size) - 1u;
    uint32_t scaled = ((uint32_t)value * max + 127u) / 255u;
    return scaled << position;
}

static uint32_t pack_rgb(uint32_t rgb) {
    uint8_t r = (uint8_t)(rgb >> 16);
    uint8_t g = (uint8_t)(rgb >> 8);
    uint8_t b = (uint8_t)rgb;
    return channel(r, red_size, red_pos) |
           channel(g, green_size, green_pos) |
           channel(b, blue_size, blue_pos);
}

static void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!enabled || x >= fb_width_value || y >= fb_height_value) return;
    ((volatile uint32_t *)(fb + y * fb_pitch))[x] = pack_rgb(color);
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
            if (rows[gy] & (1u << gx))
                framebuffer_fill_rect(x + gx * UI_FONT_SCALE, y + gy * UI_FONT_SCALE,
                                      UI_FONT_SCALE, UI_FONT_SCALE, fg);
}

int framebuffer_init(uint32_t multiboot_info) {
    enabled = 0u;
    uint8_t *cursor = (uint8_t *)(uintptr_t)(multiboot_info + 8u);
    for (;;) {
        struct mb2_tag *tag = (struct mb2_tag *)(uintptr_t)cursor;
        if (tag->type == MB2_TAG_END) break;
        if (tag->type == MB2_TAG_FRAMEBUFFER && tag->size >= sizeof(struct mb2_fb_tag)) {
            struct mb2_fb_tag *fb_tag = (struct mb2_fb_tag *)cursor;
            if (fb_tag->framebuffer_type != 1u || fb_tag->framebuffer_bpp != 32u) return -1;
            if (fb_tag->framebuffer_addr >> 32) return -1;
            fb_phys = (uint32_t)fb_tag->framebuffer_addr;
            fb_pitch = fb_tag->framebuffer_pitch;
            fb_width_value = fb_tag->framebuffer_width;
            fb_height_value = fb_tag->framebuffer_height;
            red_pos = fb_tag->red_field_position; red_size = fb_tag->red_mask_size;
            green_pos = fb_tag->green_field_position; green_size = fb_tag->green_mask_size;
            blue_pos = fb_tag->blue_field_position; blue_size = fb_tag->blue_mask_size;
            if (!fb_width_value || !fb_height_value || !fb_pitch) return -1;
            uint32_t offset = fb_phys & (PAGE_SIZE - 1u);
            uint32_t aligned = fb_phys & ~(PAGE_SIZE - 1u);
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

int framebuffer_available(void) { return enabled != 0u; }
uint32_t framebuffer_width(void) { return fb_width_value; }
uint32_t framebuffer_height(void) { return fb_height_value; }


int framebuffer_mode_supported(uint32_t width, uint32_t height) {
    if (!enabled || fb_phys == 0u) return 0;
    if (width < 640u || height < 480u || width > 4096u || height > 2160u) return 0;
    /* Probe the virtual display without changing the current mode. */
    uint16_t old_x = vbe_read(VBE_XRES), old_y = vbe_read(VBE_YRES);
    if (old_x == 0u || old_y == 0u) return 0;
    return (width <= 4096u && height <= 2160u) ? 1 : 0;
}

int framebuffer_set_mode(uint32_t width, uint32_t height) {
    if (!enabled || width < 640u || height < 480u || width > 4096u || height > 2160u) return -1;
    if (width == fb_width_value && height == fb_height_value) return 0;
    if (vbe_set_mode(width, height) != 0) return -1;

    uint32_t new_pitch = width * 4u;
    uint64_t bytes = (uint64_t)new_pitch * height;
    uint32_t mapped = (uint32_t)((bytes + PAGE_SIZE - 1u) & ~(uint64_t)(PAGE_SIZE - 1u));
    if (!mapped || mapped > FB_MAX_MAPPED_SIZE) return -1;
    uint32_t aligned = fb_phys & ~(PAGE_SIZE - 1u);
    for (uint32_t off = 0; off < mapped; off += PAGE_SIZE)
        if (paging_map_kernel_page(FB_VIRTUAL_BASE + off, aligned + off, 0x3u) != 0) return -1;

    if (desktop_buffer) { kfree(desktop_buffer); desktop_buffer = 0; }
    fb_pitch = new_pitch;
    fb_width_value = width;
    fb_height_value = height;
    desktop_mode = 0u;
    if (framebuffer_begin_desktop() != 0) return -1;
    framebuffer_clear(0x07111Fu);
    return 0;
}

int framebuffer_begin_desktop(void) {
    if (!enabled) return -1;
    if (desktop_mode) return 0;
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

void framebuffer_present(void) {
    if (!enabled || !desktop_mode || !desktop_buffer) return;
    for (uint32_t y = 0; y < fb_height_value; ++y) {
        uint32_t *dst = (uint32_t *)(fb + y * fb_pitch);
        const uint32_t *src = desktop_buffer + y * fb_width_value;
        for (uint32_t x = 0; x < fb_width_value; ++x) dst[x] = src[x];
    }
}

void framebuffer_blit_rgba32(const uint32_t *pixels, uint32_t width, uint32_t height, uint32_t x, uint32_t y, uint32_t size) {
    if (!enabled || !desktop_mode || !desktop_buffer || !pixels || !width || !height || !size) return;
    for (uint32_t dy = 0; dy < size; ++dy) {
        uint32_t sy = (dy * height) / size;
        if (y + dy >= fb_height_value) break;
        for (uint32_t dx = 0; dx < size; ++dx) {
            if (x + dx >= fb_width_value) break;
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
        uint32_t lhs=sw*height, rhs=sh*width, out_w,out_h,ox,oy;
        if(lhs>=rhs){out_w=sw;out_h=(sw*height)/width;ox=0u;oy=(sh-out_h)/2u;}
        else{out_h=sh;out_w=(sh*width)/height;ox=(sw-out_w)/2u;oy=0u;}
        for(uint32_t y=0u;y<out_h;++y){
            uint32_t sy=(y*height)/out_h;
            uint32_t *dst=wallpaper_cache+(oy+y)*sw+ox;
            for(uint32_t x=0u;x<out_w;++x){
                uint32_t sx=(x*width)/out_w;
                uint16_t p=pixels[sy*width+sx];
                uint32_t r=((p>>11)&31u)*255u/31u, g=((p>>5)&63u)*255u/63u, b=(p&31u)*255u/31u;
                dst[x]=pack_rgb((r<<16)|(g<<8)|b);
            }
        }
        wallpaper_cache_ready=1u;
    }
    framebuffer_restore_wallpaper();
}
void framebuffer_restore_wallpaper(void) {
    if (!enabled || !desktop_mode || !desktop_buffer || !wallpaper_cache || !wallpaper_cache_ready) return;
    uint32_t count=fb_width_value*fb_height_value;
    for(uint32_t i=0u;i<count;++i) desktop_buffer[i]=wallpaper_cache[i];
}

void framebuffer_clear(uint32_t color) {
    framebuffer_fill_rect(0u, 0u, fb_width_value, fb_height_value, color);
}

void framebuffer_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color) {
    if (!enabled) return;
    if (x >= fb_width_value || y >= fb_height_value) return;
    if (width > fb_width_value - x) width = fb_width_value - x;
    if (height > fb_height_value - y) height = fb_height_value - y;
    uint32_t packed = pack_rgb(color);
    for (uint32_t yy = y; yy < y + height; ++yy) {
        volatile uint32_t *line = desktop_mode ? (volatile uint32_t *)(desktop_buffer + yy * fb_width_value + x) : (volatile uint32_t *)(fb + yy * fb_pitch + x * 4u);
        for (uint32_t xx = 0; xx < width; ++xx) line[xx] = packed;
    }
}

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
