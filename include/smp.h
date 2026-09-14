#ifndef LIONOS_SMP_H
#define LIONOS_SMP_H

#include <stdint.h>

#define LIONOS_SMP_TRAMPOLINE 0x8000u
#define LIONOS_SMP_STACK_PAGES 4u
#define LIONOS_SMP_START_TIMEOUT 10000000u

void smp_init(void);
uint32_t smp_online_count(void);
uint32_t smp_lock_selftest(void);
void smp_ap_main(void);

#endif
