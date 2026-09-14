#include <stdint.h>
#include "user_libc.h"

static void print_u32(const char *label, uint32_t value) {
    char buf[11];
    uint32_t i = 0;
    if (value == 0u) {
        puts(label);
        putchar('0');
        putchar('\n');
        return;
    }
    while (value && i < sizeof(buf) - 1u) {
        buf[i++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    puts(label);
    while (i) putchar(buf[--i]);
    putchar('\n');
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    puts("LionOS userspace process test");
    print_u32("Parent PID: ", lion_getpid());

    uint32_t parent_pid = lion_getpid();
    uint32_t child_pid = lion_fork();

    if (child_pid == 0u) {
        puts("[child] fork returned 0");
        puts("[child] yielding to scheduler");
        (void)lion_yield();
        (void)lion_yield();
        puts("[child] exiting with status 42");
        lion_exit_code(42u);
    }

    if (child_pid == LIONOS_SYSCALL_ERR) {
        puts("[FAIL] fork failed");
        return 1;
    }

    print_u32("[parent] child PID: ", child_pid);
    puts("[parent] waiting for child");

    int32_t status = -1;
    int32_t waited = lion_waitpid(child_pid, &status);
    if (waited != (int32_t)child_pid) {
        puts("[FAIL] waitpid returned wrong PID");
        return 2;
    }
    if (status != 42) {
        puts("[FAIL] child exit status mismatch");
        return 3;
    }

    puts("[PASS] fork + waitpid + exit status");
    print_u32("[parent] PID before exec: ", parent_pid);
    puts("[parent] exec hello.elf; PID must remain unchanged");

    uint32_t exec_result = lion_exec("hello.elf");
    if (exec_result != parent_pid) {
        puts("[FAIL] exec PID mismatch");
        return 4;
    }

    return 5;
}
