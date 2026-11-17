#include <stdint.h>
#include "user_api.h"

int main(void) {
    int32_t status = -1;
    uint32_t child = lion_fork();
    if (child == 0u) {
        for (;;) (void)lion_yield();
    }
    if (child == LIONOS_SYSCALL_ERR ||
        lion_kill(child, LIONOS_SIG_INT) != 0)
        return 1;

    for (uint32_t attempt = 0u; attempt < 1000u; ++attempt) {
        if (lion_getstate(child) == LIONOS_PROCESS_ZOMBIE)
            break;
        (void)lion_yield();
    }
    if (lion_getstate(child) != LIONOS_PROCESS_ZOMBIE)
        return 2;
    if (lion_waitpid(child, &status) != (int32_t)child ||
        status != (int32_t)(128u + LIONOS_SIG_INT))
        return 3;

    puts("SIGINT DEFAULT ACTION: PASS");
    return 0;
}
