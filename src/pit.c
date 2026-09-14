#include <stdint.h>
#include "io.h"

void pit_init(uint32_t frequency) {
    if (frequency == 0) frequency = 100;
    uint32_t divisor = 1193182 / frequency;
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}
