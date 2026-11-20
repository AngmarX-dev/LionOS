#include <stdint.h>
#include "io.h"

void pic_init(void) {
    uint8_t master_mask = inb(0x21);
    uint8_t slave_mask = inb(0xA1);

    outb(0x20, 0x11);
    io_wait();
    outb(0xA0, 0x11);
    io_wait();
    outb(0x21, 0x20);
    io_wait();
    outb(0xA1, 0x28);
    io_wait();
    outb(0x21, 0x04);
    io_wait();
    outb(0xA1, 0x02);
    io_wait();
    outb(0x21, 0x01);
    io_wait();
    outb(0xA1, 0x01);
    io_wait();

    outb(0x21, master_mask & 0xF8u); /* IRQ0, IRQ1, IRQ2 cascade */
    outb(0xA1, (slave_mask | 0x02u) & (uint8_t)~0x10u); /* unmask IRQ12 */
}

/* Mask every legacy PIC input and verify the write. */
int pic_mask_all(void)
{
    uint8_t old_master = inb(0x21);
    uint8_t old_slave = inb(0xA1);

    outb(0x21, 0xFFu);
    io_wait();
    outb(0xA1, 0xFFu);
    io_wait();
    if (inb(0x21) == 0xFFu && inb(0xA1) == 0xFFu)
        return 0;

    /* Restore the previous routing if the mask could not be verified. */
    outb(0x21, old_master);
    io_wait();
    outb(0xA1, old_slave);
    io_wait();
    if (inb(0x21) == old_master && inb(0xA1) == old_slave)
        return -1;
    return -2;
}
