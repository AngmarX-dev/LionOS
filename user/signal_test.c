#include <stdint.h>
#include "user_api.h"
#include "user_libc.h"

static volatile uint32_t thread_observed_pgid;
static volatile uint32_t thread_observed_sid;
static volatile uint32_t thread_session_start;

static void thread_worker_pgid(void) {
    thread_observed_pgid = (uint32_t)lion_getpgid(0u);
    thread_observed_sid = (uint32_t)lion_getsid(0u);
    lion_thread_exit(0u);
}

static void thread_worker_session(void) {
    while (thread_session_start == 0u)
        (void)lion_yield();
    thread_observed_pgid = (uint32_t)lion_getpgid(0u);
    thread_observed_sid = (uint32_t)lion_getsid(0u);
    lion_thread_exit(0u);
}

static int fail(const char *message, uint32_t code) {
    puts(message);
    return (int)code;
}

static int fail_group(const char *message, uint32_t code,
                      uint32_t first, uint32_t second) {
    int32_t status = -1;
    if (first != 0u && first != LIONOS_SYSCALL_ERR) {
        if (lion_getstate(first) != LIONOS_PROCESS_ZOMBIE)
            (void)lion_kill(first, LIONOS_SIG_TERM);
        (void)lion_waitpid(first, &status);
    }
    if (second != 0u && second != LIONOS_SYSCALL_ERR) {
        if (lion_getstate(second) != LIONOS_PROCESS_ZOMBIE)
            (void)lion_kill(second, LIONOS_SIG_TERM);
        (void)lion_waitpid(second, &status);
    }
    return fail(message, code);
}

int main(void) {
    const uint32_t self = lion_getpid();
    int32_t status = -1;

    const uint32_t initial_pgid = (uint32_t)lion_getpgid(0u);
    const uint32_t initial_sid = (uint32_t)lion_getsid(0u);
    if (initial_pgid != self || initial_sid != self ||
        lion_getpgid(0xFFFFFFFFu) != -1 ||
        lion_getsid(0xFFFFFFFFu) != -1 || lion_setsid() != -1 ||
        lion_setpgid(0u, initial_pgid) != -1 ||
        lion_setpgid(0xFFFFFFFFu, 0u) != -1)
        return fail("PROCESS GROUP: self/invalid PID handling", 22);

    uint32_t tty_ready_pipe[2] = {0u, 0u};
    if (lion_pipe_create(&tty_ready_pipe[0], &tty_ready_pipe[1]) != 0)
        return fail("TTY: readiness pipe creation", 51u);
    uint32_t session_child = lion_fork();
    if (session_child == 0u) {
        (void)lion_pipe_close(tty_ready_pipe[0]);
        const uint32_t child_pid = lion_getpid();
        if (lion_getsid(0u) != (int32_t)initial_sid ||
            lion_getpgid(0u) != (int32_t)initial_pgid)
            lion_exit_code(40u);
        uint32_t session_tid = lion_thread_create(thread_worker_session);
        if (session_tid == LIONOS_SYSCALL_ERR)
            lion_exit_code(41u);
        thread_session_start = 0u;
        if (lion_setsid() != (int32_t)child_pid ||
            lion_getsid(0u) != (int32_t)child_pid ||
            lion_getpgid(0u) != (int32_t)child_pid)
            lion_exit_code(42u);
        if (lion_tty_claim() != 0 ||
            lion_tty_getpgrp() != (int32_t)child_pid)
            lion_exit_code(45u);

        struct lion_tty_attributes timed_attrs;
        uint8_t timed_byte = 0u;
        if (lion_tty_getattr(&timed_attrs) != 0)
            lion_exit_code(58u);
        uint32_t saved_tty_flags = timed_attrs.flags;
        timed_attrs.flags &= ~LIONOS_TTY_ATTR_ICANON;
        timed_attrs.vmin = 0u;
        timed_attrs.vtime = 1u;
        if (lion_tty_setattr(&timed_attrs) != 0)
            lion_exit_code(59u);
        int32_t timed_read = lion_fread(0, &timed_byte, 1u);
        timed_attrs.flags = saved_tty_flags;
        timed_attrs.vmin = 1u;
        timed_attrs.vtime = 0u;
        if (lion_tty_setattr(&timed_attrs) != 0 || timed_read != 0)
            lion_exit_code(60u);
        uint8_t tty_probe = 0u;
        static const char tty_message[] = "TTY";
        if (lion_tty_read(&tty_probe, 1u) != (int32_t)LIONOS_TTY_EMPTY ||
            lion_tty_write(tty_message, 3u) != 3 ||
            lion_fwrite(1, tty_message, 3u) != 3 ||
            lion_fwrite(2, tty_message, 3u) != 3 ||
            lion_fread(1, &tty_probe, 1u) != -1 ||
            lion_fwrite(0, tty_message, 3u) != -1 ||
            lion_tty_read((void *)(uintptr_t)0xFFFFFFFFu, 1u) != -1 ||
            lion_tty_write((const void *)(uintptr_t)0xFFFFFFFFu, 1u) != -1 ||
            lion_fread(0, (void *)(uintptr_t)0xFFFFFFFFu, 1u) != -1 ||
            lion_fwrite(1, (const void *)(uintptr_t)0xFFFFFFFFu, 1u) != -1)
            lion_exit_code(56u);
        thread_session_start = 1u;
        int32_t thread_status = -1;
        if (lion_thread_join(session_tid, &thread_status) != (int32_t)session_tid ||
            thread_status != 0 ||
            thread_observed_sid != child_pid ||
            thread_observed_pgid != child_pid)
            lion_exit_code(43u);
        uint32_t tty_group_child = lion_fork();
        if (tty_group_child == 0u) {
            if (lion_tty_claim() != -1 ||
                lion_tty_getpgrp() != (int32_t)child_pid)
                lion_exit_code(55u);
            for (;;) (void)lion_yield();
        }
        if (tty_group_child == LIONOS_SYSCALL_ERR ||
            lion_setpgid(tty_group_child, tty_group_child) != 0 ||
            lion_tty_setpgrp(tty_group_child) != 0 ||
            lion_tty_getpgrp() != (int32_t)tty_group_child ||
            lion_tty_setpgrp(initial_pgid) != -1 ||
            lion_tty_setpgrp(child_pid) != 0) {
            if (tty_group_child != 0u && tty_group_child != LIONOS_SYSCALL_ERR) {
                (void)lion_kill(tty_group_child, LIONOS_SIG_TERM);
                int32_t cleanup_status = -1;
                (void)lion_waitpid(tty_group_child, &cleanup_status);
            }
            lion_exit_code(46u);
        }
        if (lion_kill(tty_group_child, LIONOS_SIG_TERM) != 0)
            lion_exit_code(47u);
        int32_t tty_group_status = -1;
        if (lion_waitpid(tty_group_child, &tty_group_status) != (int32_t)tty_group_child)
            lion_exit_code(48u);
        const uint32_t tty_ready = 0x54545931u;
        if (lion_pipe_write(tty_ready_pipe[1], &tty_ready, sizeof(tty_ready)) !=
            (int32_t)sizeof(tty_ready))
            lion_exit_code(52u);
        (void)lion_pipe_close(tty_ready_pipe[1]);
        if (lion_setsid() != -1 || lion_setpgid(0u, child_pid) != -1)
            lion_exit_code(44u);
        for (;;) (void)lion_yield();
    }
    if (session_child == LIONOS_SYSCALL_ERR)
        return fail("SESSION: fork", 40u);
    (void)lion_pipe_close(tty_ready_pipe[1]);
    uint32_t tty_ready = 0u;
    int32_t tty_ready_bytes = lion_pipe_read(tty_ready_pipe[0], &tty_ready, sizeof(tty_ready));
    (void)lion_pipe_close(tty_ready_pipe[0]);
    uint8_t denied_tty_probe = 0u;
    static const char denied_tty_message[] = "X";
    if (lion_tty_read(&denied_tty_probe, 1u) != -1 ||
        lion_tty_write(denied_tty_message, 1u) != -1)
        return fail("TTY: cross-session access accepted", 57u);
    if (tty_ready_bytes != (int32_t)sizeof(tty_ready) ||
        tty_ready != 0x54545931u) {
        if (lion_getstate(session_child) != LIONOS_PROCESS_ZOMBIE)
            (void)lion_kill(session_child, LIONOS_SIG_TERM);
        int32_t failed_status = -1;
        (void)lion_waitpid(session_child, &failed_status);
        return fail("TTY: session claim synchronization", 53u);
    }
    uint32_t session_ready = 0u;
    for (uint32_t attempt = 0u; attempt < 10000u; ++attempt) {
        if (lion_getsid(session_child) == (int32_t)session_child) {
            session_ready = 1u;
            break;
        }
        if (lion_getstate(session_child) == LIONOS_PROCESS_ZOMBIE)
            break;
        (void)lion_yield();
    }
    if (lion_tty_claim() != -1 ||
        lion_tty_getpgrp() != -1)
        return fail("TTY: exclusive session ownership", 49u);
    if (!session_ready ||
        lion_setpgid(session_child, initial_pgid) != -1 ||
        lion_getsid(session_child) != (int32_t)session_child ||
        lion_getpgid(session_child) != (int32_t)session_child) {
        if (lion_getstate(session_child) != LIONOS_PROCESS_ZOMBIE)
            (void)lion_kill(session_child, LIONOS_SIG_TERM);
        int32_t session_status = -1;
        (void)lion_waitpid(session_child, &session_status);
        return fail("SESSION: SID creation/isolation", 41u);
    }
    if (lion_kill(session_child, LIONOS_SIG_TERM) != 0)
        return fail("SESSION: child termination", 42u);
    int32_t session_status = -1;
    if (lion_waitpid(session_child, &session_status) != (int32_t)session_child ||
        session_status != (int32_t)(128u + LIONOS_SIG_TERM))
        return fail("SESSION: child reap", 43u);
    if (lion_tty_claim() != 0 ||
        lion_tty_getpgrp() != (int32_t)self)
        return fail("TTY: release after final session process exit", 50u);
    puts("SESSION: PASS");

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
    thread_observed_sid = 0u;
    if (pgid_tid == LIONOS_SYSCALL_ERR ||
        lion_thread_join(pgid_tid, &pgid_thread_status) != (int32_t)pgid_tid ||
        pgid_thread_status != 0 || thread_observed_pgid != initial_pgid ||
        thread_observed_sid != initial_sid)
        return fail("PROCESS GROUP: thread leader lookup", 28);
    puts("PROCESS GROUP: PASS");

    uint32_t group_leader = lion_fork();
    if (group_leader == 0u) {
        for (;;) (void)lion_yield();
    }
    if (group_leader == LIONOS_SYSCALL_ERR)
        return fail("PROCESS GROUP: group leader fork", 29);
    uint32_t group_member = lion_fork();
    if (group_member == 0u) {
        for (;;) (void)lion_yield();
    }
    if (group_member == LIONOS_SYSCALL_ERR)
        return fail_group("PROCESS GROUP: group member fork", 29,
                          group_leader, 0u);
    if (lion_setpgid(group_leader, group_leader) != 0 ||
        lion_setpgid(group_member, group_leader) != 0 ||
        lion_getpgid(group_member) != (int32_t)group_leader)
        return fail_group("PROCESS GROUP: joining sibling-led group", 29,
                          group_leader, group_member);
    if (lion_killpg(group_leader, LIONOS_SIG_STOP) != 0 ||
        lion_getstate(group_leader) != LIONOS_PROCESS_STOPPED ||
        lion_getstate(group_member) != LIONOS_PROCESS_STOPPED)
        return fail_group("PROCESS GROUP: group stop", 30,
                          group_leader, group_member);
    if (lion_killpg(group_leader, LIONOS_SIG_CONT) != 0 ||
        lion_getstate(group_leader) != LIONOS_PROCESS_READY ||
        lion_getstate(group_member) != LIONOS_PROCESS_READY)
        return fail_group("PROCESS GROUP: group continue", 31,
                          group_leader, group_member);
    if (lion_killpg(0u, LIONOS_SIG_TERM) != -1 ||
        lion_killpg(0xFFFFFFFFu, LIONOS_SIG_TERM) != -1 ||
        lion_killpg(initial_pgid, LIONOS_SIG_TERM) != -1 ||
        lion_killpg(group_leader, LIONOS_SIG_MAX + 1u) != -1)
        return fail_group("PROCESS GROUP: invalid group/signal accepted", 32,
                          group_leader, group_member);
    if (lion_killpg(group_leader, LIONOS_SIG_TERM) != 0 ||
        lion_getstate(group_leader) != LIONOS_PROCESS_ZOMBIE ||
        lion_getstate(group_member) != LIONOS_PROCESS_ZOMBIE)
        return fail_group("PROCESS GROUP: group termination", 33,
                          group_leader, group_member);
    int32_t group_leader_status = -1;
    int32_t group_member_status = -1;
    if (lion_waitpid(group_leader, &group_leader_status) != (int32_t)group_leader ||
        lion_waitpid(group_member, &group_member_status) != (int32_t)group_member ||
        group_leader_status != (int32_t)(128u + LIONOS_SIG_TERM) ||
        group_member_status != (int32_t)(128u + LIONOS_SIG_TERM))
        return fail("PROCESS GROUP: group member reap", 34);

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
