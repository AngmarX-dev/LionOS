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

static int libc_self_test(void) {
    char buffer[8] = "abcde";
    char expected[8] = "aabcde";

    if (strlen(buffer) != 5u) return 1;
    if (strcmp(buffer, "abcde") != 0) return 2;
    if (strncmp(buffer, "abcdef", 5u) != 0) return 3;
    if (memcmp(buffer, "abcde", 5u) != 0) return 4;
    if (strchr(buffer, 'c') != &buffer[2]) return 5;
    if (strrchr(buffer, 'e') != &buffer[4]) return 6;
    if (atoi("  -1234x") != -1234) return 7;

    memmove(buffer + 1, buffer, 4u);
    if (memcmp(buffer, expected, 6u) != 0) return 8;

    memset(buffer, 'x', 3u);
    if (buffer[0] != 'x' || buffer[1] != 'x' || buffer[2] != 'x') return 9;
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    puts("LionOS userspace process test");

    int libc_result = libc_self_test();
    if (libc_result != 0) {
        puts("[FAIL] userspace libc self-test");
        print_u32("[FAIL] libc test code: ", (uint32_t)libc_result);
        return 10;
    }
    puts("[PASS] expanded userspace libc");

    uint32_t parent_pid = lion_getpid();
    print_u32("Parent PID: ", parent_pid);
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
