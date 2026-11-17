#include <stdint.h>
#include "futex.h"
#include "process.h"
#include "spinlock.h"
#include "usercopy.h"

#define FUTEX_BUCKET_COUNT 32u
#define FUTEX_ALIGNMENT 4u

struct futex_bucket {
    struct spinlock lock;
};

static struct futex_bucket buckets[FUTEX_BUCKET_COUNT];

static int futex_key(uint32_t address, uint32_t *channel, uint32_t *bucket_index) {
    struct process *current = process_current();
    if (!current || !channel || !bucket_index || (address & (FUTEX_ALIGNMENT - 1u)))
        return -1;
    if (!usercopy_validate_read(address, sizeof(uint32_t)))
        return -1;

    uint32_t value = address ^ (current->page_directory * 0x9E3779B9u);
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    value ^= value >> 16;
    if (!value) value = 1u;

    *channel = value;
    *bucket_index = value & (FUTEX_BUCKET_COUNT - 1u);
    return 0;
}

int32_t futex_wait(uint32_t user_address, uint32_t expected, uint32_t timeout_ticks) {
    if (timeout_ticks > 0x7FFFFFFFu) return -1;
    uint32_t channel, index;
    if (futex_key(user_address, &channel, &index) != 0)
        return -1;

    struct futex_bucket *bucket = &buckets[index];
    uint32_t flags = spinlock_irqsave_acquire(&bucket->lock);

    /* The aligned 32-bit load is atomic on x86. The bucket lock remains held
       until the process is committed to WAITING, preventing lost wakeups. */
    uint32_t observed = __atomic_load_n(
        (volatile uint32_t *)(uintptr_t)user_address, __ATOMIC_ACQUIRE);
    if (observed != expected) {
        spinlock_irqrestore_release(&bucket->lock, flags);
        return 1;
    }

    return timeout_ticks
        ? process_futex_sleep_on_timeout(channel, &bucket->lock, flags, timeout_ticks)
        : process_futex_sleep_on(channel, &bucket->lock, flags);
}

int32_t futex_wake(uint32_t user_address, uint32_t maximum) {
    uint32_t channel, index;
    if (!maximum) return 0;
    if (futex_key(user_address, &channel, &index) != 0)
        return -1;

    struct futex_bucket *bucket = &buckets[index];
    uint32_t flags = spinlock_irqsave_acquire(&bucket->lock);
    uint32_t woken = process_futex_wakeup(channel, maximum);
    spinlock_irqrestore_release(&bucket->lock, flags);
    return (int32_t)woken;
}
