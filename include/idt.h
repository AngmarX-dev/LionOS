#ifndef LIONOS_IDT_H
#define LIONOS_IDT_H

#include <stdint.h>
void idt_init(void);
void idt_load_current(void);
uint32_t *interrupt_dispatch(uint32_t *frame);

#endif
