#ifndef LIONOS_IPC_H
#define LIONOS_IPC_H

#include <stdint.h>

#define IPC_MESSAGE_MAX 128u
#define IPC_QUEUE_MAX 64u
#define IPC_RECV_EMPTY (-2)

struct ipc_message {
    uint32_t used;
    uint32_t sender_pid;
    uint32_t receiver_pid;
    uint32_t length;
    uint8_t data[IPC_MESSAGE_MAX];
};

void ipc_init(void);
int32_t ipc_send(uint32_t receiver_pid, uint32_t sender_pid, const void *data, uint32_t length);
int32_t ipc_recv(uint32_t receiver_pid, void *data, uint32_t capacity, uint32_t *sender_pid);
uint32_t ipc_pending(uint32_t receiver_pid);

#endif
