#include <stdint.h>
#include "user_libc.h"
#include "user_api.h"

static volatile uint32_t thread_marker;

static volatile uint32_t yield_marker;

static lion_mutex_t sync_test_mutex;
static lion_semaphore_t sync_test_semaphore;
static volatile uint32_t sync_test_counter;

static void thread_worker_mutex(void) {
    for (uint32_t i = 0u; i < 250u; ++i) {
        if (lion_mutex_lock(&sync_test_mutex) != 0)
            lion_thread_exit(80u);
        ++sync_test_counter;
        if (lion_mutex_unlock(&sync_test_mutex) != 0)
            lion_thread_exit(81u);
    }
    lion_thread_exit(0u);
}

static void thread_worker_semaphore(void) {
    lion_thread_exit(lion_semaphore_post(&sync_test_semaphore) == 0 ? 0u : 82u);
}

static void thread_worker_yield(void) {
    yield_marker = 0x5949454Cu;
    lion_thread_exit(17u);
}

static void thread_worker_blocking(void) {
    for (uint32_t i = 0u; i < 4u; ++i) (void)lion_yield();
    thread_marker = 0x13579BDFu;
    lion_thread_exit(73u);
}

static void thread_worker_zombie(void) {
    thread_marker = 0x2468ACE0u;
    lion_thread_exit(91u);
}

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

    /* SYS_YIELD must switch to a READY peer immediately, not wait for a
       later timer tick. Join afterwards to verify normal thread retirement. */
    uint32_t yield_tid = lion_thread_create(thread_worker_yield);
    int32_t yield_status = -1;
    if (yield_tid == LIONOS_SYSCALL_ERR) {
        puts("[FAIL] create cooperative-yield worker");
        return 30;
    }
    if (lion_yield() != LIONOS_SYSCALL_OK ||
        yield_marker != 0x5949454Cu) {
        puts("[FAIL] cooperative yield did not run peer thread");
        return 31;
    }
    if (lion_thread_join(yield_tid, &yield_status) != (int32_t)yield_tid ||
        yield_status != 17) {
        puts("[FAIL] cooperative-yield worker join");
        return 32;
    }
    puts("[PASS] cooperative yield switches to READY peer");

    /* Multiple runnable threads update one shared counter under a mutex. */
    if (lion_mutex_init(&sync_test_mutex) != 0) {
        puts("[FAIL] mutex initialization");
        return 33;
    }
    sync_test_counter = 0u;
    uint32_t mutex_tids[3];
    int32_t mutex_status = -1;
    for (uint32_t i = 0u; i < 3u; ++i) {
        mutex_tids[i] = lion_thread_create(thread_worker_mutex);
        if (mutex_tids[i] == LIONOS_SYSCALL_ERR) {
            puts("[FAIL] mutex worker creation");
            return 34;
        }
    }
    for (uint32_t i = 0u; i < 3u; ++i) {
        if (lion_thread_join(mutex_tids[i], &mutex_status) != (int32_t)mutex_tids[i] ||
            mutex_status != 0) {
            puts("[FAIL] mutex worker join");
            return 35;
        }
    }
    if (sync_test_counter != 750u || lion_mutex_trylock(&sync_test_mutex) != 0 ||
        lion_mutex_unlock(&sync_test_mutex) != 0 ||
        lion_mutex_unlock(&sync_test_mutex) == 0) {
        puts("[FAIL] mutex exclusion/trylock/unlock");
        return 36;
    }
    puts("[PASS] userspace mutex contention and exclusion");

    if (lion_semaphore_init(&sync_test_semaphore, 0u) != 0) {
        puts("[FAIL] semaphore initialization");
        return 37;
    }
    uint32_t sem_tid = lion_thread_create(thread_worker_semaphore);
    if (sem_tid == LIONOS_SYSCALL_ERR ||
        lion_semaphore_wait(&sync_test_semaphore) != 0 ||
        lion_thread_join(sem_tid, &mutex_status) != (int32_t)sem_tid ||
        mutex_status != 0) {
        puts("[FAIL] semaphore wait/post");
        return 38;
    }
    if (lion_semaphore_init(&sync_test_semaphore, UINT32_MAX) != 0 ||
        lion_semaphore_post(&sync_test_semaphore) == 0 ||
        lion_semaphore_init(&sync_test_semaphore, 1u) != 0 ||
        lion_semaphore_wait(&sync_test_semaphore) != 0 ||
        lion_semaphore_post(&sync_test_semaphore) != 0 ||
        lion_semaphore_wait(&sync_test_semaphore) != 0) {
        puts("[FAIL] semaphore overflow/count semantics");
        return 39;
    }
    puts("[PASS] userspace semaphore wait/post and overflow");

    int libc_result = libc_self_test();
    if (libc_result != 0) {
        puts("[FAIL] userspace libc self-test");
        print_u32("[FAIL] libc test code: ", (uint32_t)libc_result);
        return 10 + libc_result;
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

    /* Join a live thread: the caller blocks until the thread exits. */
    thread_marker = 0u;
    uint32_t tid = lion_thread_create(thread_worker_blocking);
    if (tid == LIONOS_SYSCALL_ERR || tid == 0u) {
        puts("[FAIL] thread creation failed");
        return 16;
    }
    int32_t thread_status = -1;
    int32_t joined = lion_thread_join(tid, &thread_status);
    if (joined != (int32_t)tid || thread_status != 73 ||
        thread_marker != 0x13579BDFu) {
        puts("[FAIL] blocking thread join result/status mismatch");
        return 17;
    }
    if (lion_thread_join(tid, &thread_status) != -1) {
        puts("[FAIL] thread join accepted an already-reaped TID");
        return 18;
    }
    if (lion_thread_join(0u, &thread_status) != -1 ||
        lion_thread_join(lion_gettid(), &thread_status) != -1) {
        puts("[FAIL] thread join accepted an invalid TID");
        return 19;
    }

    /* Let another thread exit before join, exercising the zombie path. */
    thread_marker = 0u;
    uint32_t zombie_tid = lion_thread_create(thread_worker_zombie);
    if (zombie_tid == LIONOS_SYSCALL_ERR || zombie_tid == 0u) {
        puts("[FAIL] zombie-path thread creation failed");
        return 20;
    }
    for (uint32_t i = 0u; i < 4u && thread_marker == 0u; ++i)
        (void)lion_yield();
    thread_status = -1;
    joined = lion_thread_join(zombie_tid, &thread_status);
    if (joined != (int32_t)zombie_tid || thread_status != 91 ||
        thread_marker != 0x2468ACE0u) {
        puts("[FAIL] join failed for an existing zombie thread");
        return 21;
    }

    /* Reuse the released thread slot and its virtual stack mapping. */
    thread_marker = 0u;
    tid = lion_thread_create(thread_worker_zombie);
    if (tid == LIONOS_SYSCALL_ERR || tid == 0u) {
        puts("[FAIL] thread slot/stack reuse failed");
        return 22;
    }
    for (uint32_t i = 0u; i < 4u && thread_marker == 0u; ++i)
        (void)lion_yield();
    thread_status = -1;
    joined = lion_thread_join(tid, &thread_status);
    if (joined != (int32_t)tid || thread_status != 91) {
        puts("[FAIL] reused thread stack could not be joined");
        return 23;
    }
    puts("[PASS] thread blocking join, zombie join, invalid TID, and stack reuse");

    /* Exiting a process must retire its unjoined READY threads. Repeating
       this also detects leaked process slots and stale user-stack mappings. */
    for (uint32_t i = 0u; i < 15u; ++i) {
        uint32_t owner_pid = lion_fork();
        if (owner_pid == 0u) {
            uint32_t orphan_tid = lion_thread_create(thread_worker_blocking);
            if (orphan_tid == 0u || orphan_tid == LIONOS_SYSCALL_ERR)
                lion_exit_code(101u);
            lion_exit_code(55u);
        }
        if (owner_pid == LIONOS_SYSCALL_ERR) {
            puts("[FAIL] fork failed during thread-owner cleanup stress");
            return 25;
        }
        status = -1;
        waited = lion_waitpid(owner_pid, &status);
        if (waited != (int32_t)owner_pid || status != 55) {
            puts("[FAIL] process exit did not clean up unjoined threads");
            return 26;
        }
    }
    puts("[PASS] process exit reaps unjoined threads");

    tid = lion_thread_create(thread_worker_blocking);
    if (tid == 0u || tid == LIONOS_SYSCALL_ERR) {
        puts("[FAIL] could not create thread before exec cleanup test");
        return 28;
    }
    print_u32("[parent] PID before exec: ", parent_pid);
    puts("[parent] exec hello.elf; PID must remain unchanged and sibling threads must be retired");

    uint32_t exec_result = lion_exec("hello.elf");
    if (exec_result != parent_pid) {
        puts("[FAIL] exec PID mismatch");
        return 14;
    }

    return 15;
}
