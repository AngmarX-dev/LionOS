#ifndef LIONOS_SIGNAL_H
#define LIONOS_SIGNAL_H

#include <stdint.h>

#define LIONOS_SIG_NONE 0u
#define LIONOS_SIG_INT 2u
#define LIONOS_SIG_TERM 15u
#define LIONOS_SIG_KILL 9u
#define LIONOS_SIG_STOP 19u
#define LIONOS_SIG_CONT 18u
#define LIONOS_SIG_TTIN 21u
#define LIONOS_SIG_TTOU 22u
#define LIONOS_SIG_MAX 31u

#define LIONOS_SIGNAL_OK 0
#define LIONOS_SIGNAL_ERR (-1)

void signal_init(void);
int32_t process_signal(uint32_t pid, uint32_t signal);
int32_t process_signal_group(uint32_t pgid, uint32_t signal, uint32_t sender_pid);
uint32_t process_signal_pending(uint32_t pid);
int32_t process_get_state(uint32_t pid);

#endif
