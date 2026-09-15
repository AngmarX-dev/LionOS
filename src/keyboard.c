#include <stdint.h>
#include "io.h"
#include "keyboard.h"

#define KEYBOARD_BUFFER_SIZE 128u

static volatile uint8_t buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint32_t read_index;
static volatile uint32_t write_index;
static volatile uint8_t shift_down;
static volatile uint8_t extended_prefix;

static const char keymap[128] = {
    [0x01] = 27,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x10] = 'q', [0x11] = 'w', [0x12] = 'e',
    [0x13] = 'r', [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']', [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
    [0x28] = '\'', [0x29] = '`', [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x',
    [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b', [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '/', [0x39] = ' '
};

static const char shiftmap[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+', [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E',
    [0x13] = 'R', [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}', [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L', [0x27] = ':',
    [0x28] = '"', [0x29] = '~', [0x2B] = '|', [0x2C] = 'Z', [0x2D] = 'X',
    [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B', [0x31] = 'N', [0x32] = 'M',
    [0x33] = '<', [0x34] = '>', [0x35] = '?', [0x39] = ' '
};

static void push_char(uint8_t c) {
    uint32_t next = (write_index + 1u) % KEYBOARD_BUFFER_SIZE;
    if (next == read_index) return;
    buffer[write_index] = c;
    write_index = next;
}

void keyboard_init(void) {
    read_index = 0;
    write_index = 0;
    shift_down = 0;
    extended_prefix = 0;

    /* Enable keyboard IRQ1 on the master PIC. */
    uint8_t mask = inb(0x21);
    mask &= ~(1u << 1);
    outb(0x21, mask);
}

void keyboard_handle_scancode(uint8_t scancode) {
    if (scancode == 0xE0) {
        extended_prefix = 1;
        return;
    }

    if (extended_prefix) {
        extended_prefix = 0;
        return;
    }

    if (scancode == 0x2A || scancode == 0x36) {
        shift_down = 1;
        return;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        shift_down = 0;
        return;
    }

    if (scancode & 0x80u) return;

    if (scancode == 0x0E) {
        push_char('\b');
        return;
    }

    if (scancode < 128u) {
        char c = shift_down ? shiftmap[scancode] : keymap[scancode];
        if (c) push_char((uint8_t)c);
    }
}

int keyboard_getchar(void) {
    if (read_index == write_index) return -1;
    uint8_t c = buffer[read_index];
    read_index = (read_index + 1u) % KEYBOARD_BUFFER_SIZE;
    return (int)c;
}

uint32_t keyboard_available(void) {
    if (write_index >= read_index) return write_index - read_index;
    return KEYBOARD_BUFFER_SIZE - read_index + write_index;
}
