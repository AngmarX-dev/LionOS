#include <stdint.h>
#include "user_api.h"

#define SIGNAL_READY 0x53494752u

static volatile uint32_t signal_seen;
static volatile uint32_t waitpid_signal_seen;

static void on_sigint(uint32_t signal) {
    if (signal == LIONOS_SIG_INT)
        signal_seen = 1u;
}

static void on_waitpid_sigint(uint32_t signal) {
    if (signal == LIONOS_SIG_INT)
        waitpid_signal_seen = 1u;
}

static void reap_failed_child(uint32_t child) {
    int32_t status = -1;
    (void)lion_waitpid(child, &status);
}

static int32_t run_waitpid_eintr_test(void) {
    waitpid_signal_seen = 0u;
    if (lion_sigaction(LIONOS_SIG_INT, on_waitpid_sigint) != 0)
        return 20;
    uint32_t waiter = lion_fork();
    if (waiter == LIONOS_SYSCALL_ERR)
        return 21;
    if (waiter == 0u) {
        uint32_t grandchild = lion_fork();
        if (grandchild == LIONOS_SYSCALL_ERR)
            lion_exit_code(22u);
        if (grandchild == 0u) {
            for (;;)
                (void)lion_yield();
        }
        int32_t status = -1;
        if (lion_waitpid(grandchild, &status) != LIONOS_EINTR)
            lion_exit_code(23u);
        if (!waitpid_signal_seen)
            lion_exit_code(24u);
        if (lion_kill(grandchild, LIONOS_SIG_TERM) != 0)
            lion_exit_code(25u);
        if (lion_waitpid(grandchild, &status) != (int32_t)grandchild)
            lion_exit_code(26u);
        lion_exit_code(0u);
    }
    uint32_t waiting = 0u;
    for (uint32_t attempt = 0u; attempt < 2000u; ++attempt) {
        if (lion_getstate(waiter) == LIONOS_PROCESS_STATE_WAITING) {
            waiting = 1u;
            break;
        }
        (void)lion_yield();
    }
    if (!waiting) {
        (void)lion_kill(waiter, LIONOS_SIG_TERM);
        int32_t status = -1;
        (void)lion_waitpid(waiter, &status);
        return 27;
    }
    if (lion_kill(waiter, LIONOS_SIG_INT) != 0) {
        (void)lion_kill(waiter, LIONOS_SIG_TERM);
        int32_t status = -1;
        (void)lion_waitpid(waiter, &status);
        return 28;
    }
    int32_t status = -1;
    if (lion_waitpid(waiter, &status) != (int32_t)waiter || status != 0)
        return 29;
    return 0;
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

    int32_t wait_result = run_waitpid_eintr_test();
    if (wait_result != 0)
        return wait_result;

    return 0;
}
