#include <stdint.h>
#include "console.h"

#define VGA_WIDTH 80u
#define VGA_HEIGHT 25u

static volatile uint16_t *const VGA = (uint16_t *)0xB8000;
static uint32_t row;
static uint32_t col;
static uint8_t color;

static void scroll(void) {
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
    console_clear();
}

void console_clear(void) {
    for (uint32_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; ++i)
        VGA[i] = ((uint16_t)color << 8) | ' ';
    row = 0;
    col = 0;
}

void console_set_color(uint8_t fg) {
    color = (uint8_t)(fg & 0x0Fu);
}

uint8_t console_color(void) {
    return color;
}

void console_putc(char c) {
    if (c == '\n') { col = 0; ++row; scroll(); return; }
    if (c == '\r') { col = 0; return; }
    if (c == '\b') {
        if (col) --col;
        VGA[row * VGA_WIDTH + col] = ((uint16_t)color << 8) | ' ';
        return;
    }
    VGA[row * VGA_WIDTH + col] = ((uint16_t)color << 8) | (uint8_t)c;
    if (++col >= VGA_WIDTH) { col = 0; ++row; scroll(); }
}

void console_write(const char *s) { while (*s) console_putc(*s++); }

void console_write_n(const char *s, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) console_putc(s[i]);
}

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
