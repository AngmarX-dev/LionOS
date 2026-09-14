#ifndef LIONOS_TSS_H
#define LIONOS_TSS_H

#include <stdint.h>

void tss_init(void);
void tss_init_cpu(uint32_t cpu, uint32_t stack_top);
void tss_set_kernel_stack(uint32_t stack_top);

#endif
