#ifndef LIONOS_IDT_H
#define LIONOS_IDT_H

#include <stdint.h>
void idt_init(void);
void interrupt_dispatch(uint32_t *frame);

#endif
