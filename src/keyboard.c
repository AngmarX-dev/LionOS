#include "io.h"

void keyboard_init(void) {
    /* Enable keyboard IRQ1 on the master PIC. */
    uint8_t mask = inb(0x21);
    mask &= ~(1u << 1);
    outb(0x21, mask);
}
