#include <stdint.h>
#include "user_api.h"
#include "user_libc.h"

int main(void) {
    uint32_t child = lion_fork();
    if (child == 0u) {
        static const char message[] = "hello from child via LionOS IPC";
        uint32_t parent = lion_getppid();
        int32_t sent = lion_ipc_send(parent, message, (uint32_t)(sizeof(message) - 1u));
        lion_exit_code(sent == (int32_t)(sizeof(message) - 1u) ? 0u : 1u);
    }
    if (child == (uint32_t)-1) {
        puts("IPC TEST: FAIL (fork)");
        return 1;
    }

    char buffer[IPC_MESSAGE_MAX + 1u];
    uint32_t sender = 0;
    for (uint32_t spins = 0; spins < 100000u && lion_ipc_pending() == 0u; ++spins) lion_yield();
    int32_t received = lion_ipc_recv(buffer, IPC_MESSAGE_MAX, &sender);
    if (received < 0) {
        puts("IPC TEST: FAIL (receive)");
        return 1;
    }
    buffer[(uint32_t)received] = 0;
    int32_t status = 0;
    int32_t waited = lion_waitpid(child, &status);
    if (waited != (int32_t)child || status != 0 || sender != child) {
        puts("IPC TEST: FAIL (process state)");
        return 1;
    }
    puts("IPC TEST: PASS");
    puts(buffer);
    return 0;
}
