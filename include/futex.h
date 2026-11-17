#ifndef LIONOS_FUTEX_H
#define LIONOS_FUTEX_H

#include <stdint.h>

#define LIONOS_FUTEX_BLOCKED (-2)

int32_t futex_wait(uint32_t user_address, uint32_t expected);
int32_t futex_wake(uint32_t user_address, uint32_t maximum);

#endif
