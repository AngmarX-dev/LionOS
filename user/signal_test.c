#include <stdint.h>
#include "user_api.h"
#include "user_libc.h"

static volatile uint32_t thread_observed_pgid;

static void thread_worker_pgid(void) {
    thread_observed_pgid = (uint32_t)lion_getpgid(0u);
    lion_thread_exit(0u);
}

static int fail(const char *message, uint32_t code) {
    puts(message);
    return (int)code;
}

int main(void) {
    const uint32_t self = lion_getpid();
    int32_t status = -1;

    const uint32_t initial_pgid = (uint32_t)lion_getpgid(0u);
    if (initial_pgid != self || lion_getpgid(0xFFFFFFFFu) != -1 ||
        lion_setpgid(0u, initial_pgid) != 0 ||
        lion_setpgid(0xFFFFFFFFu, 0u) != -1)
        return fail("PROCESS GROUP: self/invalid PID handling", 22);

    uint32_t pgid_child = lion_fork();
    if (pgid_child == 0u) {
        for (;;) (void)lion_yield();
    }
    if (pgid_child == LIONOS_SYSCALL_ERR ||
        lion_getpgid(pgid_child) != (int32_t)initial_pgid)
        return fail("PROCESS GROUP: fork inheritance", 23);
    if (lion_setpgid(pgid_child, pgid_child) != 0 ||
        lion_getpgid(pgid_child) != (int32_t)pgid_child)
        return fail("PROCESS GROUP: child group creation", 24);
    if (lion_setpgid(pgid_child, initial_pgid) != 0 ||
        lion_getpgid(pgid_child) != (int32_t)initial_pgid)
        return fail("PROCESS GROUP: child rejoin", 25);
    if (lion_setpgid(pgid_child, 0xFFFFFFFFu) != -1 ||
        lion_getpgid(pgid_child) != (int32_t)initial_pgid ||
        lion_kill(pgid_child, LIONOS_SIG_TERM) != 0)
        return fail("PROCESS GROUP: rejected update/child cleanup", 26);
    int32_t pgid_status = -1;
    if (lion_waitpid(pgid_child, &pgid_status) != (int32_t)pgid_child ||
        pgid_status != (int32_t)(128u + LIONOS_SIG_TERM))
        return fail("PROCESS GROUP: child reap", 27);

    uint32_t pgid_tid = lion_thread_create(thread_worker_pgid);
    int32_t pgid_thread_status = -1;
    thread_observed_pgid = 0u;
    if (pgid_tid == LIONOS_SYSCALL_ERR ||
        lion_thread_join(pgid_tid, &pgid_thread_status) != (int32_t)pgid_tid ||
        pgid_thread_status != 0 || thread_observed_pgid != initial_pgid)
        return fail("PROCESS GROUP: thread leader lookup", 28);
    puts("PROCESS GROUP: PASS");

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

    if (lion_getstate(child) != LIONOS_PROCESS_READY)
        return fail("SIGNAL TEST: child was not READY", 4);
    if (lion_kill(child, 0u) != -1 ||
        lion_kill(child, LIONOS_SIG_MAX + 1u) != -1)
        return fail("SIGNAL TEST: invalid signal accepted", 5);

    if (lion_kill(child, LIONOS_SIG_STOP) != 0)
        return fail("SIGNAL TEST: SIGSTOP failed", 6);
    if (lion_getstate(child) != LIONOS_PROCESS_STOPPED)
        return fail("SIGNAL TEST: SIGSTOP state mismatch", 7);
    uint32_t stop_mask = 1u << (LIONOS_SIG_STOP - 1u);
    if ((lion_sigpending(child) & stop_mask) == 0u)
        return fail("SIGNAL TEST: SIGSTOP pending bit missing", 8);

    if (lion_kill(child, LIONOS_SIG_CONT) != 0)
        return fail("SIGNAL TEST: SIGCONT failed", 9);
    if (lion_getstate(child) != LIONOS_PROCESS_READY)
        return fail("SIGNAL TEST: SIGCONT did not resume child", 10);
    if ((lion_sigpending(child) & stop_mask) != 0u)
        return fail("SIGNAL TEST: SIGCONT did not clear STOP bit", 11);

    if (lion_kill(child, LIONOS_SIG_TERM) != 0)
        return fail("SIGNAL TEST: SIGTERM failed", 12);
    if (lion_getstate(child) != LIONOS_PROCESS_ZOMBIE)
        return fail("SIGNAL TEST: SIGTERM did not terminate child", 13);
    uint32_t term_mask = 1u << (LIONOS_SIG_TERM - 1u);
    if ((lion_sigpending(child) & term_mask) == 0u)
        return fail("SIGNAL TEST: SIGTERM pending bit missing", 14);
    if (lion_kill(child, LIONOS_SIG_CONT) != -1)
        return fail("SIGNAL TEST: zombie process accepted a signal", 15);
    int32_t waited = lion_waitpid(child, &status);
    if (waited != (int32_t)child ||
        status != (int32_t)(128u + LIONOS_SIG_TERM))
        return fail("SIGNAL TEST: SIGTERM wait status mismatch", 16);

    child = lion_fork();
    if (child == 0u) {
        for (;;) lion_yield();
    }
    if (child == (uint32_t)-1)
        return fail("SIGNAL TEST: fork for SIGKILL failed", 16);
    if (lion_kill(child, LIONOS_SIG_KILL) != 0)
        return fail("SIGNAL TEST: SIGKILL failed", 17);
    if (lion_getstate(child) != LIONOS_PROCESS_ZOMBIE)
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
