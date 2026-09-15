#include <stdint.h>
#include "console.h"
#include "framebuffer.h"

#define VGA_WIDTH 80u
#define VGA_HEIGHT 25u
#define FB_CELL_W 12u
#define FB_CELL_H 16u
#define FB_LEFT 16u
#define FB_TOP 52u
#define FB_BOTTOM 48u

static volatile uint16_t *const VGA = (uint16_t *)0xB8000;
static uint32_t row;
static uint32_t col;
static uint8_t color;
static uint32_t graphics;

static const uint32_t palette[16] = {
    0x000000u, 0x2244AAu, 0x2ECC71u, 0x1ABC9Cu,
    0xE74C3Cu, 0x9B59B6u, 0x9A6B30u, 0xD6DEEBu,
    0x5B6B7Au, 0x3498DBu, 0x66FF99u, 0x66FFFFu,
    0xFF6B6Bu, 0xFF7AC8u, 0xF2C14Eu, 0xF5F7FAu
};

static uint32_t fg_rgb(void) { return palette[color & 0x0Fu]; }
static uint32_t bg_rgb(void) { return 0x07111Fu; }

static uint32_t fb_rows(void) {
    uint32_t h = framebuffer_height();
    return h > FB_TOP + FB_BOTTOM ? (h - FB_TOP - FB_BOTTOM) / FB_CELL_H : 0u;
}

static uint32_t fb_cols(void) {
    uint32_t w = framebuffer_width();
    return w > FB_LEFT ? (w - FB_LEFT) / FB_CELL_W : 0u;
}

static void vga_scroll(void) {
    if (row < VGA_HEIGHT) return;
    for (uint32_t y = 1; y < VGA_HEIGHT; ++y)
        for (uint32_t x = 0; x < VGA_WIDTH; ++x)
            VGA[(y - 1u) * VGA_WIDTH + x] = VGA[y * VGA_WIDTH + x];
    for (uint32_t x = 0; x < VGA_WIDTH; ++x)
        VGA[(VGA_HEIGHT - 1u) * VGA_WIDTH + x] = ((uint16_t)color << 8) | ' ';
    row = VGA_HEIGHT - 1u;
}

void console_init(void) {
    color = CONSOLE_COLOR_LIGHT_GRAY;
    graphics = 0u;
    console_clear();
}

void console_clear(void) {
    if (graphics) {
        framebuffer_console_clear();
        row = 0;
        col = 0;
        return;
    }
    for (uint32_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; ++i)
        VGA[i] = ((uint16_t)color << 8) | ' ';
    row = 0;
    col = 0;
}

void console_set_color(uint8_t fg) {
    color = (uint8_t)(fg & 0x0Fu);
}

uint8_t console_color(void) { return color; }

void console_use_framebuffer(void) {
    if (!framebuffer_available()) return;
    graphics = 1u;
    console_clear();
}

void console_putc(char c) {
    if (graphics) {
        uint32_t rows = fb_rows();
        uint32_t cols = fb_cols();
        if (!rows || !cols) return;
        if (c == '\n') {
            col = 0;
            ++row;
            if (row >= rows) console_clear();
            return;
        }
        if (c == '\r') { col = 0; return; }
        if (c == '\b') {
            if (col) --col;
            framebuffer_console_putc(' ', row, col, bg_rgb(), bg_rgb());
            return;
        }
        framebuffer_console_putc(c, row, col, fg_rgb(), bg_rgb());
        if (++col >= cols) {
            col = 0;
            ++row;
            if (row >= rows) console_clear();
        }
        return;
    }
    if (c == '\n') { col = 0; ++row; vga_scroll(); return; }
    if (c == '\r') { col = 0; return; }
    if (c == '\b') {
        if (col) --col;
        VGA[row * VGA_WIDTH + col] = ((uint16_t)color << 8) | ' ';
        return;
    }
    VGA[row * VGA_WIDTH + col] = ((uint16_t)color << 8) | (uint8_t)c;
    if (++col >= VGA_WIDTH) { col = 0; ++row; vga_scroll(); }
}

void console_write(const char *s) { while (*s) console_putc(*s++); }
void console_write_n(const char *s, uint32_t n) { for (uint32_t i = 0; i < n; ++i) console_putc(s[i]); }

void console_write_dec(uint32_t value) {
    char buf[11]; uint32_t n = 0;
    if (!value) { console_putc('0'); return; }
    while (value && n < sizeof(buf)) { buf[n++] = (char)('0' + value % 10u); value /= 10u; }
    while (n) console_putc(buf[--n]);
}

void console_write_hex(uint32_t value) {
    static const char hex[] = "0123456789ABCDEF";
    console_write("0x");
    for (int i = 7; i >= 0; --i) console_putc(hex[(value >> (i * 4)) & 0xFu]);
}

uint32_t console_row(void) { return row; }
uint32_t console_col(void) { return col; }
