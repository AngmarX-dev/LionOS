#ifndef LIONOS_PIT_H
#define LIONOS_PIT_H

#include <stdint.h>

void pit_init(uint32_t frequency);
void pit_disable_timer(void);

#endif
