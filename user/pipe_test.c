#include <stdint.h>
#include "user_api.h"
#include "user_libc.h"

static volatile uint32_t pipe_zombie_marker;

static void pipe_zombie_worker(void) {
    __atomic_store_n(&pipe_zombie_marker, 1u, __ATOMIC_RELEASE);
    lion_thread_exit(0u);
}

static void pipe_live_worker(void) {
    for (;;) (void)lion_yield();
}

int main(void) {
    uint32_t read_end = 0u;
    uint32_t write_end = 0u;
    static const char message[] = "pipe fork inheritance";
    char buffer[sizeof(message)];
    uint32_t child;
    int32_t status = -1;

    if (lion_pipe_create(&read_end, &write_end) != 0) return 1;
    child = lion_fork();
    if (child == (uint32_t)-1) return 2;

    if (child == 0u) {
        if (lion_pipe_close(read_end) != 0) lion_exit_code(3u);
        if (lion_pipe_write(write_end, message, sizeof(message)) !=
            (int32_t)sizeof(message)) lion_exit_code(4u);
        /* Process exit must release this inherited writer automatically. */
        lion_exit_code(0u);
    }

    if (lion_pipe_close(write_end) != 0) return 5;
    int32_t read_result = lion_pipe_read(read_end, buffer, sizeof(buffer));
    if (read_result != (int32_t)sizeof(message)) {
        int32_t child_status = -1;
        (void)lion_pipe_close(read_end);
        if (lion_waitpid(child, &child_status) == (int32_t)child &&
            child_status >= 0 && child_status <= 20) {
            if (read_result == 0) return 70 + child_status;
            if (read_result == -1) return 90 + child_status;
            if (read_result == -2) return 110 + child_status;
            if (read_result > 0 && read_result <= 20)
                return 130 + (read_result * 21) + child_status;
            return 600 + child_status;
        }
        if (read_result == 0) return 60;
        if (read_result == -1) return 61;
        if (read_result == -2) return 62;
        return 63;
    }
    if (memcmp(buffer, message, sizeof(message)) != 0) return 7;
    if (lion_pipe_read(read_end, buffer, sizeof(buffer)) != 0) return 8;
    if (lion_pipe_close(read_end) != 0) return 9;
    if (lion_waitpid(child, &status) != (int32_t)child || status != 0)
        return 10;

    /*
     * Keep both parent endpoints open while children inherit them. Each child
     * first retires one zombie sibling, then exits with two live siblings.
     * Process teardown must retire those threads before dropping only the
     * child's pipe references; the parent's endpoints must remain usable.
     */
    if (lion_pipe_create(&read_end, &write_end) != 0) return 11;
    for (uint32_t i = 0u; i < 64u; ++i) {
        child = lion_fork();
        if (child == (uint32_t)-1) return 12;
        if (child == 0u) {
            __atomic_store_n(&pipe_zombie_marker, 0u, __ATOMIC_RELEASE);
            uint32_t zombie_tid = lion_thread_create(pipe_zombie_worker);
            if (zombie_tid == 0u || zombie_tid == LIONOS_SYSCALL_ERR)
                lion_exit_code(16u);
            for (uint32_t turn = 0u; turn < 16u &&
                 __atomic_load_n(&pipe_zombie_marker, __ATOMIC_ACQUIRE) == 0u;
                 ++turn)
                (void)lion_yield();
            if (__atomic_load_n(&pipe_zombie_marker, __ATOMIC_ACQUIRE) == 0u)
                lion_exit_code(17u);

            for (uint32_t sibling = 0u; sibling < 2u; ++sibling) {
                uint32_t live_tid = lion_thread_create(pipe_live_worker);
                if (live_tid == 0u || live_tid == LIONOS_SYSCALL_ERR)
                    lion_exit_code(18u + sibling);
            }
            lion_exit_code(0u);
        }

        status = -1;
        if (lion_waitpid(child, &status) != (int32_t)child || status != 0)
            return 13;

        char expected = (char)(i ^ 0x5Au);
        char observed = 0;
        if (lion_pipe_write(write_end, &expected, 1u) != 1 ||
            lion_pipe_read(read_end, &observed, 1u) != 1 ||
            observed != expected)
            return 14;
    }
    if (lion_pipe_close(read_end) != 0 || lion_pipe_close(write_end) != 0)
        return 15;

    puts("PIPE TEST: PASS");
    puts("PIPE TEST: parent references survive 64 child exits with sibling cleanup");
    return 0;
}
