#include <stdint.h>
#include "user_libc.h"
#include "user_api.h"

static volatile uint32_t worker_mask;
static volatile uint32_t worker_ticket;
static volatile uint32_t worker_sink[2];

static void cpu_bound_worker(void) {
    uint32_t ticket = __atomic_fetch_add(&worker_ticket, 1u, __ATOMIC_ACQ_REL);
    if (ticket >= 2u) lion_thread_exit(1u);

    volatile uint32_t value = 0x13579BDFu ^ ticket;
    for (uint32_t i = 0u; i < 2000000u; ++i)
        value = (value * 1664525u) + 1013904223u + i;
    worker_sink[ticket] = value;
    __atomic_fetch_or(&worker_mask, 1u << ticket, __ATOMIC_RELEASE);
    lion_thread_exit(0u);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    worker_mask = 0u;
    worker_ticket = 0u;
    worker_sink[0] = 0u;
    worker_sink[1] = 0u;

    uint32_t tids[2];
    tids[0] = lion_thread_create(cpu_bound_worker);
    tids[1] = lion_thread_create(cpu_bound_worker);
    if (tids[0] == 0u || tids[0] == LIONOS_SYSCALL_ERR ||
        tids[1] == 0u || tids[1] == LIONOS_SYSCALL_ERR) {
        puts("[FAIL] timer-preemption worker creation");
        return 1;
    }

    volatile uint32_t parent_work = 0xA5A5A5A5u;
    for (uint32_t i = 0u; i < 32000000u; ++i)
        parent_work = (parent_work * 1664525u) + 1013904223u + i;
    (void)parent_work;

    if (__atomic_load_n(&worker_mask, __ATOMIC_ACQUIRE) != 3u) {
        puts("[FAIL] timer did not preempt parent for both peers");
        return 2;
    }

    for (uint32_t i = 0u; i < 2u; ++i) {
        int32_t status = -1;
        if (lion_thread_join(tids[i], &status) != (int32_t)tids[i] || status != 0) {
            puts("[FAIL] timer-preemption worker join");
            return 3;
        }
    }
    puts("[PASS] timer preemption runs CPU-bound peers");

    volatile uint32_t timeout_word = 1u;
    for (uint32_t i = 0u; i < 8u; ++i) {
        if (lion_futex_wait_timeout(&timeout_word, 1u, 4u) !=
            LIONOS_FUTEX_TIMED_OUT) {
            puts("[FAIL] repeated scheduler timer timeout");
            return 4;
        }
    }
    puts("[PASS] repeated timer timeout and scheduler wakeup");
    return 0;
}
