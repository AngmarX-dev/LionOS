#ifndef LIONOS_GDT_H
#define LIONOS_GDT_H

#include <stdint.h>

void gdt_init(void);
void gdt_set_tss(uint32_t base, uint32_t limit);

#endif
