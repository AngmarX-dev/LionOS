#include <stdint.h>
#include "io.h"
#include "mouse.h"

#define PS2_STATUS       0x64u
#define PS2_COMMAND      0x64u
#define PS2_DATA         0x60u
#define PS2_AUX_DISABLE  0xA7u
#define PS2_AUX_ENABLE   0xA8u
#define PS2_READ_CONFIG  0x20u
#define PS2_WRITE_CONFIG 0x60u
#define PS2_WRITE_AUX    0xD4u
#define MOUSE_SET_DEFAULTS 0xF6u
#define MOUSE_ENABLE_STREAMING 0xF4u
#define MOUSE_ACK        0xFAu
#define MOUSE_PACKET_SIZE 3u
#define VGA_WIDTH 80u
#define VGA_HEIGHT 25u
#define CURSOR_GLYPH 0xDBu
#define CURSOR_COLOR 0x1Eu /* yellow on blue */

static volatile uint16_t *const vga = (volatile uint16_t *)0xB8000;
static uint32_t initialized;
static uint32_t cursor_visible;
static uint32_t cursor_enabled = 1u;
static uint32_t cursor_x_pos = VGA_WIDTH / 2u;
static uint32_t cursor_y_pos = VGA_HEIGHT / 2u;
static uint16_t cursor_saved_cell;
static uint8_t packet[MOUSE_PACKET_SIZE];
static uint32_t packet_index;
static uint8_t current_buttons;

static int wait_input_clear(void) {
    for (uint32_t i = 0; i < 100000u; ++i) {
        if ((inb(PS2_STATUS) & 0x02u) == 0u) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

static int wait_output(void) {
    for (uint32_t i = 0; i < 100000u; ++i) {
        if (inb(PS2_STATUS) & 0x01u) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

static void flush_output(void) {
    for (uint32_t i = 0; i < 32u && (inb(PS2_STATUS) & 0x01u); ++i)
        (void)inb(PS2_DATA);
}

static int controller_command(uint8_t command) {
    if (wait_input_clear() < 0) return -1;
    outb(PS2_COMMAND, command);
    return 0;
}

static int mouse_command(uint8_t command) {
    if (controller_command(PS2_WRITE_AUX) < 0) return -1;
    if (wait_input_clear() < 0) return -1;
    outb(PS2_DATA, command);
    if (wait_output() < 0) return -1;
    return inb(PS2_DATA) == MOUSE_ACK ? 0 : -1;
}

static void cursor_save(void) {
    cursor_saved_cell = vga[cursor_y_pos * VGA_WIDTH + cursor_x_pos];
}

void mouse_hide(void) {
    if (!cursor_visible) return;
    vga[cursor_y_pos * VGA_WIDTH + cursor_x_pos] = cursor_saved_cell;
    cursor_visible = 0u;
}

void mouse_show(void) {
    if (!cursor_enabled || !initialized || cursor_visible) return;
    cursor_save();
    vga[cursor_y_pos * VGA_WIDTH + cursor_x_pos] = ((uint16_t)CURSOR_COLOR << 8) | CURSOR_GLYPH;
    cursor_visible = 1u;
}

void mouse_set_cursor_visible(uint32_t visible) {
    cursor_enabled = visible ? 1u : 0u;
    if (!cursor_enabled) mouse_hide();
}

static void cursor_move(int32_t dx, int32_t dy) {
    int32_t nx = (int32_t)cursor_x_pos + dx;
    int32_t ny = (int32_t)cursor_y_pos + dy;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx >= (int32_t)VGA_WIDTH) nx = (int32_t)VGA_WIDTH - 1;
    if (ny >= (int32_t)VGA_HEIGHT) ny = (int32_t)VGA_HEIGHT - 1;
    if ((uint32_t)nx == cursor_x_pos && (uint32_t)ny == cursor_y_pos) return;
    mouse_hide();
    cursor_x_pos = (uint32_t)nx;
    cursor_y_pos = (uint32_t)ny;
    mouse_show();
}

static void handle_packet(void) {
    uint8_t status = packet[0];
    if ((status & 0x08u) == 0u) return;
    if (status & 0x40u || status & 0x80u) {
        current_buttons = status & 0x07u;
        return;
    }

    int32_t dx = (int32_t)(int8_t)packet[1];
    int32_t dy = (int32_t)(int8_t)packet[2];
    current_buttons = status & 0x07u;

    /* Use a finer text-grid response so the GUI cursor feels responsive. */
    int32_t cell_dx = dx / 2;
    int32_t cell_dy = -(dy / 2);
    if (dx > 0 && cell_dx == 0) cell_dx = 1;
    if (dx < 0 && cell_dx == 0) cell_dx = -1;
    if (dy > 0 && cell_dy == 0) cell_dy = -1;
    if (dy < 0 && cell_dy == 0) cell_dy = 1;
    cursor_move(cell_dx, cell_dy);
}

int mouse_init(void) {
    initialized = 0u;
    cursor_enabled = 1u;
    cursor_visible = 0u;
    packet_index = 0u;
    current_buttons = 0u;

    /* Keep mouse input polled instead of adding IRQ12 while the UI is still young. */
    (void)controller_command(PS2_AUX_DISABLE);
    flush_output();
    if (controller_command(PS2_READ_CONFIG) < 0) return -1;
    if (wait_output() < 0) return -1;
    uint8_t config = inb(PS2_DATA);
    config &= (uint8_t)~0x02u; /* mouse IRQ disabled; polling is used */
    if (controller_command(PS2_WRITE_CONFIG) < 0) return -1;
    if (wait_input_clear() < 0) return -1;
    outb(PS2_DATA, config);
    if (controller_command(PS2_AUX_ENABLE) < 0) return -1;
    flush_output();

    if (mouse_command(MOUSE_SET_DEFAULTS) < 0) return -1;
    if (mouse_command(MOUSE_ENABLE_STREAMING) < 0) return -1;

    initialized = 1u;
    mouse_show();
    return 0;
}

void mouse_poll(void) {
    if (!initialized) return;
    while (inb(PS2_STATUS) & 0x01u) {
        uint8_t status = inb(PS2_STATUS);
        if ((status & 0x20u) == 0u) break;
        uint8_t value = inb(PS2_DATA);
        if (packet_index == 0u && (value & 0x08u) == 0u) continue;
        packet[packet_index++] = value;
        if (packet_index == MOUSE_PACKET_SIZE) {
            packet_index = 0u;
            handle_packet();
        }
    }
}

uint32_t mouse_x(void) { return cursor_x_pos; }
uint32_t mouse_y(void) { return cursor_y_pos; }
uint8_t mouse_buttons(void) { return current_buttons; }
