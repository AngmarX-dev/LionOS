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
    (void)lion_waitpid(child, &status);
}

int main(void) {
    uint32_t parent = lion_getpid();
    if (parent == 0u)
        return 1;

    if (lion_sigaction(LIONOS_SIG_INT, on_sigint) != 0)
        return 2;
    if (lion_sigaction(LIONOS_SIG_KILL, on_sigint) == 0)
        return 3;

    lion_signal_handler_t invalid_handler =
        (lion_signal_handler_t)(uintptr_t)0x20000000u;
    if (lion_sigaction(LIONOS_SIG_INT, invalid_handler) == 0)
        return 4;

    uint32_t child = lion_fork();
    if (child == LIONOS_SYSCALL_ERR)
        return 5;

    if (child == 0u) {
        if (lion_sigaction(LIONOS_SIG_HUP, LIONOS_SIG_IGN) != 0)
            lion_exit_code(8u);

        uint32_t ready = SIGNAL_READY;
        int32_t sent = lion_ipc_send(parent, &ready, sizeof(ready));
        if (sent != (int32_t)sizeof(ready))
            lion_exit_code(7u);

        for (uint32_t attempt = 0u; attempt < 1000u; ++attempt) {
            if (signal_seen)
                lion_exit_code(0u);
            (void)lion_yield();
        }
        lion_exit_code(6u);
    }

    uint32_t ready = 0u;
    uint32_t sender = 0u;
    int32_t received = (int32_t)LIONOS_IPC_EMPTY;
    for (uint32_t attempt = 0u; attempt < 1000u; ++attempt) {
        received = lion_ipc_recv(&ready, sizeof(ready), &sender);
        if (received != (int32_t)LIONOS_IPC_EMPTY)
            break;
        (void)lion_yield();
    }
    if (received != (int32_t)sizeof(ready) ||
        sender != child || ready != SIGNAL_READY) {
        reap_failed_child(child);
        return 9;
    }

    if (lion_kill(child, LIONOS_SIG_HUP) != 0) {
        reap_failed_child(child);
        return 10;
    }

    if (lion_kill(child, LIONOS_SIG_INT) != 0) {
        reap_failed_child(child);
        return 11;
    }

    int32_t status = -1;
    if (lion_waitpid(child, &status) != (int32_t)child)
        return 12;
    if (status != 0)
        return 13;

    return 0;
}
