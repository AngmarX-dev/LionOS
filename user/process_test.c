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

    memmove(buffer + 1, buffer, 5u);
    if (memcmp(buffer, expected, 6u) != 0) return 8;

    memset(buffer, 'x', 3u);
    if (buffer[0] != 'x' || buffer[1] != 'x' || buffer[2] != 'x') return 9;
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    puts("LionOS userspace process lifecycle test");

    int libc_result = libc_self_test();
    if (libc_result != 0) {
        puts("[FAIL] userspace libc self-test");
        print_u32("[FAIL] libc test code: ", (uint32_t)libc_result);
        return 10;
    }
    puts("[PASS] expanded userspace libc");

    uint32_t parent_pid = lion_getpid();
    print_u32("Parent PID: ", parent_pid);

    /* Exercise the blocking wait path: the child yields before exiting. */
    uint32_t child_pid = lion_fork();
    if (child_pid == 0u) {
        puts("[child] blocking-wait case");
        (void)lion_yield();
        (void)lion_yield();
        lion_exit_code(42u);
    }
    if (child_pid == LIONOS_SYSCALL_ERR) {
        puts("[FAIL] fork failed in blocking-wait case");
        return 1;
    }

    int32_t status = -1;
    int32_t waited = lion_waitpid(child_pid, &status);
    if (waited != (int32_t)child_pid || status != 42) {
        puts("[FAIL] blocking waitpid result/status mismatch");
        return 2;
    }
    puts("[PASS] blocking waitpid + exit status");

    /* A reaped child and a non-child must both be rejected. */
    if (lion_waitpid(child_pid, &status) != -1) {
        puts("[FAIL] waitpid accepted an already-reaped child");
        return 3;
    }
    if (lion_waitpid(parent_pid, &status) != -1) {
        puts("[FAIL] waitpid accepted the caller as its own child");
        return 4;
    }

    /* Yield once so this child can become a zombie before waitpid is called. */
    child_pid = lion_fork();
    if (child_pid == 0u) lion_exit_code(51u);
    if (child_pid == LIONOS_SYSCALL_ERR) {
        puts("[FAIL] fork failed in zombie-before-wait case");
        return 5;
    }
    (void)lion_yield();
    status = -1;
    waited = lion_waitpid(child_pid, &status);
    if (waited != (int32_t)child_pid || status != 51) {
        puts("[FAIL] waitpid failed for an existing zombie");
        return 6;
    }
    puts("[PASS] waitpid reaps an existing zombie");

    /* Wait-any must return exactly one child and preserve its exit status. */
    uint32_t child_code = 61u;
    uint32_t first_pid = lion_fork();
    if (first_pid == 0u) lion_exit_code(child_code);
    if (first_pid == LIONOS_SYSCALL_ERR) {
        puts("[FAIL] first wait-any fork failed");
        return 7;
    }

    child_code = 62u;
    uint32_t second_pid = lion_fork();
    if (second_pid == 0u) lion_exit_code(child_code);
    if (second_pid == LIONOS_SYSCALL_ERR) {
        (void)lion_waitpid(first_pid, &status);
        puts("[FAIL] second wait-any fork failed");
        return 8;
    }

    (void)lion_yield();
    status = -1;
    waited = lion_waitpid(0u, &status);
    if (waited == (int32_t)first_pid) {
        if (status != 61) {
            puts("[FAIL] wait-any returned wrong first-child status");
            return 9;
        }
    } else if (waited == (int32_t)second_pid) {
        if (status != 62) {
            puts("[FAIL] wait-any returned wrong second-child status");
            return 10;
        }
    } else {
        puts("[FAIL] wait-any returned an unrelated PID");
        return 11;
    }

    uint32_t remaining_pid = (waited == (int32_t)first_pid)
        ? second_pid : first_pid;
    int32_t remaining_status = (remaining_pid == first_pid) ? 61 : 62;
    status = -1;
    waited = lion_waitpid(remaining_pid, &status);
    if (waited != (int32_t)remaining_pid || status != remaining_status) {
        puts("[FAIL] waitpid failed for the remaining child");
        return 12;
    }
    if (lion_waitpid(0u, &status) != -1) {
        puts("[FAIL] wait-any succeeded after all children were reaped");
        return 13;
    }

    puts("[PASS] waitpid blocking, zombie, wait-any, and invalid-child cases");
    print_u32("[parent] PID before exec: ", parent_pid);
    puts("[parent] exec hello.elf; PID must remain unchanged");

    uint32_t exec_result = lion_exec("hello.elf");
    if (exec_result != parent_pid) {
        puts("[FAIL] exec PID mismatch");
        return 14;
    }

    return 15;
}
