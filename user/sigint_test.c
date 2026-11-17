#include <stdint.h>
#include "user_api.h"

int main(void) {
    int32_t status = -1;
    uint32_t child = lion_fork();
    if (child == 0u) {
        for (;;) (void)lion_yield();
    }
    if (child == LIONOS_SYSCALL_ERR)
        return 1;
    if (lion_kill(child, LIONOS_SIG_INT) != 0) {
        (void)lion_kill(child, LIONOS_SIG_TERM);
        (void)lion_waitpid(child, &status);
        return 2;
    }

    uint32_t exited = 0u;
    for (uint32_t attempt = 0u; attempt < 1000u; ++attempt) {
        if (lion_getstate(child) == LIONOS_PROCESS_ZOMBIE) {
            exited = 1u;
            break;
        }
        (void)lion_yield();
    }
    if (!exited) {
        (void)lion_kill(child, LIONOS_SIG_TERM);
        (void)lion_waitpid(child, &status);
        return 3;
    }
    if (lion_waitpid(child, &status) != (int32_t)child ||
        status != (int32_t)(128u + LIONOS_SIG_INT))
        return 4;

    puts("SIGINT DEFAULT ACTION: PASS");
    return 0;
}
