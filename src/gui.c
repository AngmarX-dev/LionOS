#include <stdint.h>
#include "debug.h"
#include "framebuffer.h"
#include "gui.h"
#include "keyboard.h"
#include "mouse.h"

#define GRID_W 80u
#define GRID_H 48u
#define CELL_W 12u
#define CELL_H 16u
#define GRID_X 32u
#define GRID_Y 12u

#define C_BG       0x08111Fu
#define C_PANEL    0x101F35u
#define C_PANEL2   0x172B46u
#define C_BORDER   0x2B5F8Au
#define C_ACCENT   0xF2C14Eu
#define C_TEXT     0xE8EEF7u
#define C_DIM      0x9CB0C9u
#define C_GOOD     0x55D187u
#define C_BAD      0xE46A76u
#define C_CURSOR   0xFFFFFFu

static const uint8_t font[26][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    {0x0F,0x10,0x10,0x10,0x10,0x10,0x0F},
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x0F,0x10,0x10,0x17,0x11,0x11,0x0F},
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F},
    {0x1F,0x02,0x02,0x02,0x12,0x12,0x0C},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}
};

static void draw_char(char c, uint32_t x, uint32_t y, uint32_t color) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c < 'A' || c > 'Z') return;
    const uint8_t *rows = font[(uint32_t)(c - 'A')];
    for (uint32_t gy = 0; gy < 7u; ++gy) {
        for (uint32_t gx = 0; gx < 5u; ++gx) {
            if (rows[gy] & (1u << (4u - gx)))
                framebuffer_fill_rect(x + gx * 2u, y + gy * 2u, 2u, 2u, color);
        }
    }
}

static void draw_text(const char *text, uint32_t col, uint32_t row, uint32_t color) {
    uint32_t x = GRID_X + col * CELL_W;
    uint32_t y = GRID_Y + row * CELL_H;
    while (*text && col < GRID_W) {
        if (*text != ' ') draw_char(*text, x, y + 1u, color);
        x += CELL_W;
        ++col;
        ++text;
    }
}

static void button(uint32_t col, uint32_t row, uint32_t width, uint32_t height,
                   uint32_t fill, const char *label) {
    uint32_t x = GRID_X + col * CELL_W;
    uint32_t y = GRID_Y + row * CELL_H;
    uint32_t w = width * CELL_W;
    uint32_t h = height * CELL_H;
    framebuffer_fill_rect(x, y, w, h, C_BORDER);
    framebuffer_fill_rect(x + 2u, y + 2u, w - 4u, h - 4u, fill);
    uint32_t text_col = col + (width > 2u ? (width - 2u) / 2u : 0u);
    if (label[0] && label[1]) {
        uint32_t len = 0;
        while (label[len]) ++len;
        if (len < width) text_col = col + (width - len) / 2u;
    }
    draw_text(label, text_col, row + 1u, C_TEXT);
}

static void draw_cursor(uint32_t mx, uint32_t my) {
    uint32_t x = mx * CELL_W + 4u;
    uint32_t y = my * CELL_H + 4u;
    framebuffer_fill_rect(x, y, 3u, 18u, C_CURSOR);
    framebuffer_fill_rect(x, y, 10u, 3u, C_CURSOR);
    framebuffer_fill_rect(x + 4u, y + 14u, 6u, 3u, C_CURSOR);
}

static void draw_desktop(uint32_t mx, uint32_t my, uint32_t about_open, const char *status) {
    framebuffer_clear(C_BG);
    uint32_t width = framebuffer_width();
    uint32_t height = framebuffer_height();

    framebuffer_fill_rect(0, 0, width, 44u, C_PANEL2);
    framebuffer_fill_rect(0, height - 44u, width, 44u, C_PANEL2);
    framebuffer_fill_rect(0, 44u, width, height - 88u, C_BG);

    draw_text("LIONOS DESKTOP", 2u, 1u, C_ACCENT);
    draw_text("PHASE 26", 67u, 1u, C_DIM);
    draw_text("READY", 2u, 45u, C_GOOD);
    draw_text("ESC: SHELL", 60u, 45u, C_DIM);

    framebuffer_fill_rect(GRID_X, GRID_Y, 72u * CELL_W, 20u * CELL_H, C_BORDER);
    framebuffer_fill_rect(GRID_X + 3u, GRID_Y + 3u, 72u * CELL_W - 6u, 20u * CELL_H - 6u, C_PANEL);
    draw_text("WELCOME TO LIONOS", 3u, 15u, C_TEXT);
    draw_text("SMP AND STORAGE ARE ONLINE", 3u, 18u, C_DIM);
    draw_text("GRAPHICAL INPUT IS ENABLED", 3u, 20u, C_DIM);
    draw_text(status, 3u, 23u, C_GOOD);

    button(8u, 28u, 18u, 4u, 0x1D4164u, "TERMINAL");
    button(31u, 28u, 15u, 4u, 0x35506Fu, "ABOUT");
    button(51u, 28u, 14u, 4u, 0x5B2A34u, "EXIT");

    if (about_open) {
        framebuffer_fill_rect(GRID_X + 10u * CELL_W, GRID_Y + 8u * CELL_H,
                              58u * CELL_W, 20u * CELL_H, C_BORDER);
        framebuffer_fill_rect(GRID_X + 10u * CELL_W + 3u, GRID_Y + 8u * CELL_H + 3u,
                              58u * CELL_W - 6u, 20u * CELL_H - 6u, C_PANEL2);
        draw_text("LIONOS", 14u, 11u, C_ACCENT);
        draw_text("32 BIT X86 EXPERIMENTAL OS", 14u, 14u, C_TEXT);
        draw_text("MULTIBOOT2 GRUB BOOT", 14u, 16u, C_TEXT);
        draw_text("SMP MEMORY PROCESS VFS", 14u, 18u, C_TEXT);
        draw_text("MOUSE POLLING INPUT", 14u, 20u, C_TEXT);
        draw_text("PRESS ABOUT AGAIN OR ESC", 14u, 23u, C_DIM);
    }

    draw_cursor(mx, my);
}

void gui_run(void) {
    debug_write("LIONOS:GUI-ENTER\n");

    if (!framebuffer_available()) {
        debug_write("LIONOS:GUI-NO-FRAMEBUFFER\n");
        return;
    }

    mouse_set_cursor_visible(0u);
    while (keyboard_available()) (void)keyboard_getchar();

    uint32_t about_open = 0u;
    uint32_t previous_buttons = mouse_buttons();
    uint32_t mx = (mouse_x() * 80u) / 80u;
    uint32_t my = (mouse_y() * 48u) / 25u;
    if (my >= GRID_H) my = GRID_H - 1u;
    const char *status = "GRAPHICS ONLINE";
    draw_desktop(mx, my, about_open, status);

    for (;;) {
        mouse_poll();
        uint32_t next_mx = mouse_x();
        uint32_t next_my = (mouse_y() * 48u) / 25u;
        if (next_mx >= GRID_W) next_mx = GRID_W - 1u;
        if (next_my >= GRID_H) next_my = GRID_H - 1u;

        int redraw = (next_mx != mx || next_my != my);
        mx = next_mx;
        my = next_my;

        uint32_t buttons = mouse_buttons();
        if ((buttons & 1u) && !(previous_buttons & 1u)) {
            if (mx >= 8u && mx < 26u && my >= 28u && my < 32u) {
                status = "OPENING SHELL";
                draw_desktop(mx, my, 0u, status);
                debug_write("LIONOS:GUI-TERMINAL\n");
                mouse_set_cursor_visible(1u);
                mouse_show();
                debug_write("LIONOS:GUI-EXIT\n");
                return;
            }
            if (mx >= 31u && mx < 46u && my >= 28u && my < 32u) {
                about_open = about_open ? 0u : 1u;
                redraw = 1;
            }
            if (mx >= 51u && mx < 65u && my >= 28u && my < 32u) {
                status = "RETURNING TO SHELL";
                mouse_set_cursor_visible(1u);
                mouse_show();
                debug_write("LIONOS:GUI-EXIT\n");
                return;
            }
        }
        previous_buttons = buttons;

        while (keyboard_available()) {
            int ch = keyboard_getchar();
            if (ch == 27 || ch == 'q' || ch == 'Q') {
                mouse_set_cursor_visible(1u);
                mouse_show();
                debug_write("LIONOS:GUI-EXIT\n");
                return;
            }
            if (ch == 'a' || ch == 'A') {
                about_open = about_open ? 0u : 1u;
                redraw = 1;
            }
            if (ch == 't' || ch == 'T') {
                mouse_set_cursor_visible(1u);
                mouse_show();
                debug_write("LIONOS:GUI-EXIT\n");
                return;
            }
        }

        if (redraw) draw_desktop(mx, my, about_open, status);
        __asm__ volatile("pause");
    }
}
