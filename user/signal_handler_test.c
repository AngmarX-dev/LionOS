#include <stdint.h>
#include "user_api.h"

#define SIGNAL_READY 0x53494752u

static volatile uint32_t signal_seen;

static void on_sigint(uint32_t signal) {
    if (signal == LIONOS_SIG_INT)
        signal_seen = 1u;
}

static void reap_failed_child(uint32_t child) {
    int32_t status = -1;
    (void)lion_kill(child, LIONOS_SIG_TERM);
    (void)lion_waitpid(child, &status);
}

int main(void) {
    uint32_t parent = lion_getpid();
    if (parent == 0u)
        return 1;

    if (lion_sigaction(LIONOS_SIG_INT, on_sigint) != 0)
        return 2;

    uint32_t child = lion_fork();
    if (child == LIONOS_SYSCALL_ERR)
        return 3;

    if (child == 0u) {
        uint32_t ready = SIGNAL_READY;
        if (lion_ipc_send(parent, &ready, sizeof(ready)) != 0)
            lion_exit_code(4u);

        for (uint32_t attempt = 0u; attempt < 1000u; ++attempt) {
            if (signal_seen)
                lion_exit_code(0u);
            (void)lion_yield();
        }
        lion_exit_code(5u);
    }

    uint32_t ready = 0u;
    uint32_t sender = 0u;
    int32_t received = lion_ipc_recv_blocking(&ready, sizeof(ready), &sender);
    if (received != (int32_t)sizeof(ready) ||
        sender != child || ready != SIGNAL_READY) {
        reap_failed_child(child);
        return 6;
    }

    if (lion_kill(child, LIONOS_SIG_INT) != 0) {
        reap_failed_child(child);
        return 7;
    }

    int32_t status = -1;
    if (lion_waitpid(child, &status) != (int32_t)child)
        return 8;
    if (status != 0)
        return 9;

    return 0;
}
