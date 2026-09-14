#ifndef LIONOS_GDT_H
#define LIONOS_GDT_H

#include <stdint.h>

void gdt_init(void);
void gdt_set_tss(uint32_t base, uint32_t limit);
void gdt_load_cpu(uint32_t cpu, uint32_t tss_base, uint32_t tss_limit);
uint32_t gdt_user_code_selector(void);
uint32_t gdt_user_data_selector(void);
uint32_t gdt_tss_selector(void);

#endif
