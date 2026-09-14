#ifndef LIONOS_LAPIC_H
#define LIONOS_LAPIC_H

#include <stdint.h>

#define LIONOS_LAPIC_PHYS 0xFEE00000u
#define LIONOS_LAPIC_VIRT 0xFEE00000u

int lapic_init(void);
uint32_t lapic_id(void);
void lapic_eoi(void);
void lapic_enable(void);

#endif
