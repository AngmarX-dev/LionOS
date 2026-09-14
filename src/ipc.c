#include <stdint.h>
#include "ipc.h"

static struct ipc_message messages[IPC_QUEUE_MAX];

void ipc_init(void) {
    for (uint32_t i = 0; i < IPC_QUEUE_MAX; ++i) messages[i].used = 0;
}

int32_t ipc_send(uint32_t receiver_pid, uint32_t sender_pid, const void *data, uint32_t length) {
    if (!receiver_pid || !data || !length || length > IPC_MESSAGE_MAX) return -1;
    for (uint32_t i = 0; i < IPC_QUEUE_MAX; ++i) {
        if (messages[i].used) continue;
        const uint8_t *src = (const uint8_t *)data;
        messages[i].used = 1;
        messages[i].sender_pid = sender_pid;
        messages[i].receiver_pid = receiver_pid;
        messages[i].length = length;
        for (uint32_t j = 0; j < length; ++j) messages[i].data[j] = src[j];
        return (int32_t)length;
    }
    return -1;
}

int32_t ipc_recv(uint32_t receiver_pid, void *data, uint32_t capacity, uint32_t *sender_pid) {
    if (!receiver_pid || !data || !capacity) return -1;
    int found = -1;
    for (uint32_t i = 0; i < IPC_QUEUE_MAX; ++i) {
        if (messages[i].used && messages[i].receiver_pid == receiver_pid) {
            found = (int)i;
            break;
        }
    }
    if (found < 0) return IPC_RECV_EMPTY;
    struct ipc_message *m = &messages[found];
    uint32_t n = m->length < capacity ? m->length : capacity;
    uint8_t *dst = (uint8_t *)data;
    for (uint32_t j = 0; j < n; ++j) dst[j] = m->data[j];
    if (sender_pid) *sender_pid = m->sender_pid;
    m->used = 0;
    return (int32_t)n;
}

uint32_t ipc_pending(uint32_t receiver_pid) {
    uint32_t count = 0;
    if (!receiver_pid) return 0;
    for (uint32_t i = 0; i < IPC_QUEUE_MAX; ++i)
        if (messages[i].used && messages[i].receiver_pid == receiver_pid) ++count;
    return count;
}
