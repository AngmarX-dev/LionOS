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

    outb(0x21, master_mask & 0xFC);
    outb(0xA1, slave_mask | 0x02);
}
