#include <stdint.h>
#include "user_api.h"
#include "user_libc.h"

static volatile uint32_t wake_word;
static volatile uint32_t completed;

static void futex_waiter(void) {
    if (lion_futex_wait_timeout(&wake_word, 0u, 2000u) != 0)
        lion_thread_exit(1u);
    __atomic_add_fetch(&completed, 1u, __ATOMIC_RELEASE);
    lion_thread_exit(0u);
}

static int run_wake_limit_test(void) {
    uint32_t tids[2];
    int32_t status = -1;
    uint32_t created = 0u;
    wake_word = 0u;
    completed = 0u;

    for (; created < 2u; ++created) {
        tids[created] = lion_thread_create(futex_waiter);
        if (tids[created] == LIONOS_SYSCALL_ERR || tids[created] == 0u) {
            (void)lion_futex_wake(&wake_word, UINT32_MAX);
            for (uint32_t i = 0u; i < created; ++i)
                (void)lion_thread_join(tids[i], &status);
            return 1;
        }
    }

    for (uint32_t expected = 1u; expected <= 2u; ++expected) {
        uint32_t spins = 0u;
        int32_t woken = 0;
        while (spins++ < 100000u) {
            woken = lion_futex_wake(&wake_word, 1u);
            if (woken != 0) break;
            (void)lion_yield();
        }
        if (woken != 1) {
            (void)lion_futex_wake(&wake_word, UINT32_MAX);
            for (uint32_t i = 0u; i < 2u; ++i)
                (void)lion_thread_join(tids[i], &status);
            return 2;
        }

        spins = 0u;
        while (__atomic_load_n(&completed, __ATOMIC_ACQUIRE) < expected &&
               spins++ < 100000u)
            (void)lion_yield();
        if (__atomic_load_n(&completed, __ATOMIC_ACQUIRE) != expected) {
            (void)lion_futex_wake(&wake_word, UINT32_MAX);
            for (uint32_t i = 0u; i < 2u; ++i)
                (void)lion_thread_join(tids[i], &status);
            return 3;
        }
    }

    for (uint32_t i = 0u; i < 2u; ++i) {
        if (lion_thread_join(tids[i], &status) != (int32_t)tids[i] ||
            status != 0)
            return 4;
    }
    return 0;
}

int main(void) {
    int result = run_wake_limit_test();
    if (result != 0) {
        puts("FUTEX WAKE LIMIT TEST: FAIL");
        return 50 + result;
    }
    puts("FUTEX WAKE LIMIT TEST: PASS");

    if (lion_exec("hello.elf") == LIONOS_SYSCALL_ERR)
        return 54;
    return 55;
}
