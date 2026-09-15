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
#define C_CURSOR   0xFFFFFFu

static const uint8_t font[26][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    {0x0F,0x10,0x10,0x10,0x10,0x10,0x0F}, {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x0F,0x10,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}, {0x1F,0x02,0x02,0x02,0x12,0x12,0x0C},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}
};

static void draw_char(char c, uint32_t x, uint32_t y, uint32_t color) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c < 'A' || c > 'Z') return;
    const uint8_t *rows = font[(uint32_t)(c - 'A')];
    for (uint32_t gy = 0; gy < 7u; ++gy)
        for (uint32_t gx = 0; gx < 5u; ++gx)
            if (rows[gy] & (1u << (4u - gx)))
                framebuffer_fill_rect(x + gx * 2u, y + gy * 2u, 2u, 2u, color);
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
    uint32_t len = 0u;
    while (label[len]) ++len;
    uint32_t text_col = col + (len < width ? (width - len) / 2u : 0u);
    draw_text(label, text_col, row + 1u, C_TEXT);
}

static void draw_cursor(uint32_t mx, uint32_t my) {
    uint32_t x = mx * CELL_W + 4u;
    uint32_t y = my * CELL_H + 4u;
    framebuffer_fill_rect(x, y, 3u, 18u, C_CURSOR);
    framebuffer_fill_rect(x, y, 10u, 3u, C_CURSOR);
    framebuffer_fill_rect(x + 4u, y + 14u, 6u, 3u, C_CURSOR);
}

static void draw_desktop(uint32_t mx, uint32_t my, const char *status) {
    framebuffer_clear(C_BG);
    uint32_t width = framebuffer_width();
    uint32_t height = framebuffer_height();
    framebuffer_fill_rect(0, 0, width, 44u, C_PANEL2);
    framebuffer_fill_rect(0, height - 44u, width, 44u, C_PANEL2);
    framebuffer_fill_rect(0, 44u, width, height - 88u, C_BG);
    draw_text("LIONOS DESKTOP", 2u, 1u, C_ACCENT);
    draw_text("PHASE 26", 67u, 1u, C_DIM);
    draw_text("READY", 2u, 45u, C_GOOD);
    draw_text("ESC SHELL", 61u, 45u, C_DIM);
    framebuffer_fill_rect(GRID_X, GRID_Y, 72u * CELL_W, 20u * CELL_H, C_BORDER);
    framebuffer_fill_rect(GRID_X + 3u, GRID_Y + 3u, 72u * CELL_W - 6u, 20u * CELL_H - 6u, C_PANEL);
    draw_text("WELCOME TO LIONOS", 3u, 15u, C_TEXT);
    draw_text("SMP AND STORAGE ONLINE", 3u, 18u, C_DIM);
    draw_text("GRAPHICAL INPUT ENABLED", 3u, 20u, C_DIM);
    draw_text(status, 3u, 23u, C_GOOD);
    button(8u, 28u, 18u, 4u, 0x1D4164u, "TERMINAL");
    button(31u, 28u, 15u, 4u, 0x35506Fu, "ABOUT");
    button(51u, 28u, 14u, 4u, 0x5B2A34u, "EXIT");
    draw_cursor(mx, my);
}

static void draw_about(uint32_t mx, uint32_t my) {
    uint32_t x = GRID_X + 10u * CELL_W;
    uint32_t y = GRID_Y + 8u * CELL_H;
    uint32_t w = 58u * CELL_W;
    uint32_t h = 20u * CELL_H;
    framebuffer_fill_rect(x, y, w, h, C_BORDER);
    framebuffer_fill_rect(x + 3u, y + 3u, w - 6u, h - 6u, C_PANEL2);
    draw_text("LIONOS", 14u, 11u, C_ACCENT);
    draw_text("32 BIT X86 EXPERIMENTAL OS", 14u, 14u, C_TEXT);
    draw_text("MULTIBOOT2 GRUB BOOT", 14u, 16u, C_TEXT);
    draw_text("SMP MEMORY PROCESS VFS", 14u, 18u, C_TEXT);
    draw_text("MOUSE POLLING INPUT", 14u, 20u, C_TEXT);
    draw_text("PRESS A OR Q TO CLOSE", 14u, 23u, C_DIM);
    button(62u, 9u, 4u, 2u, 0x5B2A34u, "X");
    draw_cursor(mx, my);
}

static void return_shell(void) {
    mouse_set_cursor_visible(1u);
    mouse_show();
    debug_write("LIONOS:GUI-EXIT\n");
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
    uint32_t mx = mouse_x();
    uint32_t my = (mouse_y() * GRID_H) / 25u;
    if (mx >= GRID_W) mx = GRID_W - 1u;
    if (my >= GRID_H) my = GRID_H - 1u;
    const char *status = "GRAPHICS ONLINE";
    draw_desktop(mx, my, status);

    for (;;) {
        keyboard_poll();
        mouse_poll();

        uint32_t next_mx = mouse_x();
        uint32_t next_my = (mouse_y() * GRID_H) / 25u;
        if (next_mx >= GRID_W) next_mx = GRID_W - 1u;
        if (next_my >= GRID_H) next_my = GRID_H - 1u;
        int redraw = (next_mx != mx || next_my != my) && !about_open;
        mx = next_mx;
        my = next_my;

        uint32_t buttons = mouse_buttons();
        if ((buttons & 1u) && !(previous_buttons & 1u)) {
            if (about_open) {
                if (mx >= 62u && mx < 66u && my >= 9u && my < 11u) {
                    about_open = 0u;
                    redraw = 1;
                    debug_write("LIONOS:GUI-ABOUT-CLOSE-MOUSE\n");
                } else if (mx < 10u || mx >= 68u || my < 8u || my >= 28u) {
                    about_open = 0u;
                    redraw = 1;
                    debug_write("LIONOS:GUI-ABOUT-CLOSE-OUTSIDE\n");
                }
            } else if (mx >= 8u && mx < 26u && my >= 28u && my < 32u) {
                return_shell();
                return;
            } else if (mx >= 31u && mx < 46u && my >= 28u && my < 32u) {
                about_open = 1u;
                redraw = 1;
                debug_write("LIONOS:GUI-ABOUT-OPEN\n");
            } else if (mx >= 51u && mx < 65u && my >= 28u && my < 32u) {
                return_shell();
                return;
            }
        }
        previous_buttons = buttons;

        while (keyboard_available()) {
            int ch = keyboard_getchar();
            if (ch == 27 || ch == 'q' || ch == 'Q') {
                if (about_open) {
                    about_open = 0u;
                    redraw = 1;
                    debug_write("LIONOS:GUI-ABOUT-CLOSE-KEY\n");
                } else {
                    return_shell();
                    return;
                }
            } else if (ch == 'a' || ch == 'A') {
                about_open = about_open ? 0u : 1u;
                redraw = 1;
                debug_write("LIONOS:GUI-ABOUT-KEY\n");
            } else if (ch == 't' || ch == 'T') {
                return_shell();
                return;
            }
        }

        if (redraw) {
            draw_desktop(mx, my, status);
            if (about_open) draw_about(mx, my);
        }
        if (about_open) {
            /* Keep the modal static while servicing input; do not redraw the full framebuffer for pointer motion. */
            draw_about(mx, my);
        }
        __asm__ volatile("pause");
    }
}
