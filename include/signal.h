#ifndef LIONOS_SIGNAL_H
#define LIONOS_SIGNAL_H

#include <stdint.h>

#define LIONOS_SIG_NONE 0u
#define LIONOS_SIG_TERM 15u
#define LIONOS_SIG_KILL 9u
#define LIONOS_SIG_STOP 19u
#define LIONOS_SIG_CONT 18u
#define LIONOS_SIG_MAX 31u

#define LIONOS_SIGNAL_OK 0
#define LIONOS_SIGNAL_ERR (-1)

int32_t process_signal(uint32_t pid, uint32_t signal);
uint32_t process_signal_pending(uint32_t pid);
int32_t process_get_state(uint32_t pid);

#endif
