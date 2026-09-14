#include <stdint.h>
#include "user_api.h"
#include "user_libc.h"

int main(void) {
    uint32_t child = lion_fork();
    if (child == 0u) {
        for (;;) lion_yield();
    }
    if (child == (uint32_t)-1) {
        puts("SIGNAL TEST: FAIL (fork)");
        return 1;
    }

    if (lion_getstate(child) != 1) {
        puts("SIGNAL TEST: FAIL (initial state)");
        return 1;
    }
    if (lion_kill(child, LIONOS_SIG_TERM) != 0) {
        puts("SIGNAL TEST: FAIL (SIGTERM)");
        return 1;
    }
    int32_t status = 0;
    int32_t waited = lion_waitpid(child, &status);
    if (waited != (int32_t)child || status != (int32_t)(128u + LIONOS_SIG_TERM)) {
        puts("SIGNAL TEST: FAIL (wait/status)");
        return 1;
    }
    puts("SIGNAL TEST: PASS");
    return 0;
}
