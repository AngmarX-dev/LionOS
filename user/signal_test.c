#include <stdint.h>
#include "user_api.h"
#include "user_libc.h"

#define PROCESS_READY 1
#define PROCESS_ZOMBIE 3
#define PROCESS_STOPPED 5

static int fail(const char *message, uint32_t code) {
    puts(message);
    return (int)code;
}

int main(void) {
    const uint32_t self = lion_getpid();
    int32_t status = -1;

    if (lion_kill(self, LIONOS_SIG_TERM) != -1)
        return fail("SIGNAL TEST: self-signal must be rejected", 1);
    if (lion_kill(0u, LIONOS_SIG_TERM) != -1)
        return fail("SIGNAL TEST: zero PID must be rejected", 2);

    uint32_t child = lion_fork();
    if (child == 0u) {
        for (;;) lion_yield();
    }
    if (child == (uint32_t)-1)
        return fail("SIGNAL TEST: fork for STOP/CONT failed", 3);

    if (lion_getstate(child) != PROCESS_READY)
        return fail("SIGNAL TEST: child was not READY", 4);
    if (lion_kill(child, 0u) != -1 ||
        lion_kill(child, LIONOS_SIG_MAX + 1u) != -1)
        return fail("SIGNAL TEST: invalid signal accepted", 5);

    if (lion_kill(child, LIONOS_SIG_STOP) != 0)
        return fail("SIGNAL TEST: SIGSTOP failed", 6);
    if (lion_getstate(child) != PROCESS_STOPPED)
        return fail("SIGNAL TEST: SIGSTOP state mismatch", 7);
    uint32_t stop_mask = 1u << (LIONOS_SIG_STOP - 1u);
    if ((lion_sigpending(child) & stop_mask) == 0u)
        return fail("SIGNAL TEST: SIGSTOP pending bit missing", 8);

    if (lion_kill(child, LIONOS_SIG_CONT) != 0)
        return fail("SIGNAL TEST: SIGCONT failed", 9);
    if (lion_getstate(child) != PROCESS_READY)
        return fail("SIGNAL TEST: SIGCONT did not resume child", 10);
    if ((lion_sigpending(child) & stop_mask) != 0u)
        return fail("SIGNAL TEST: SIGCONT did not clear STOP bit", 11);

    if (lion_kill(child, LIONOS_SIG_TERM) != 0)
        return fail("SIGNAL TEST: SIGTERM failed", 12);
    if (lion_getstate(child) != PROCESS_ZOMBIE)
        return fail("SIGNAL TEST: SIGTERM did not terminate child", 13);
    uint32_t term_mask = 1u << (LIONOS_SIG_TERM - 1u);
    if ((lion_sigpending(child) & term_mask) == 0u)
        return fail("SIGNAL TEST: SIGTERM pending bit missing", 14);
    int32_t waited = lion_waitpid(child, &status);
    if (waited != (int32_t)child ||
        status != (int32_t)(128u + LIONOS_SIG_TERM))
        return fail("SIGNAL TEST: SIGTERM wait status mismatch", 15);

    child = lion_fork();
    if (child == 0u) {
        for (;;) lion_yield();
    }
    if (child == (uint32_t)-1)
        return fail("SIGNAL TEST: fork for SIGKILL failed", 16);
    if (lion_kill(child, LIONOS_SIG_KILL) != 0)
        return fail("SIGNAL TEST: SIGKILL failed", 17);
    if (lion_getstate(child) != PROCESS_ZOMBIE)
        return fail("SIGNAL TEST: SIGKILL did not terminate child", 18);
    uint32_t kill_mask = 1u << (LIONOS_SIG_KILL - 1u);
    if ((lion_sigpending(child) & kill_mask) == 0u)
        return fail("SIGNAL TEST: SIGKILL pending bit missing", 19);
    status = -1;
    waited = lion_waitpid(child, &status);
    if (waited != (int32_t)child ||
        status != (int32_t)(128u + LIONOS_SIG_KILL))
        return fail("SIGNAL TEST: SIGKILL wait status mismatch", 20);

    if (lion_waitpid(0u, &status) != -1)
        return fail("SIGNAL TEST: wait-any found an already reaped child", 21);

    puts("SIGNAL TEST: PASS");
    return 0;
}
