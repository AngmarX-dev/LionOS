#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include "memory.h"
#include "spinlock.h"
#include "debug.h"
#include "multiboot2.h"

#define PAGE_SIZE 4096ULL
#define MIB (1024ULL * 1024ULL)
#define LOW_AVAILABLE_START (64ULL * MIB)
#define LOW_AVAILABLE_END (80ULL * MIB)
#define LOW_RESERVED_START (68ULL * MIB)
#define LOW_RESERVED_END (72ULL * MIB)
#define HIGH_AVAILABLE_START (512ULL * MIB)
#define HIGH_AVAILABLE_END (520ULL * MIB)
#define LOW_MEMORY_LIMIT (256ULL * MIB)
/* The low allocator permanently reserves the first 8 MiB. */
#define LOW_RESERVED_PAGES (8ULL * MIB / PAGE_SIZE)
#define HIGH_MEMORY_LIMIT (64ULL * 1024ULL * MIB)
#define HIGH_RESERVED_START (514ULL * MIB)
#define HIGH_RESERVED_END (515ULL * MIB)
#define MODULE_START (74ULL * MIB)
#define MODULE_END (75ULL * MIB)
#define MEMORY_STRESS_THREADS 8u
#define MEMORY_STRESS_HELD_PAGES 4u
#define MEMORY_STRESS_ITERATIONS 1000u
#define LOW_OWNER_SLOTS ((LOW_AVAILABLE_END - LOW_AVAILABLE_START) / PAGE_SIZE)
#define HIGH_OWNER_SLOTS ((HIGH_AVAILABLE_END - HIGH_AVAILABLE_START) / PAGE_SIZE)

/*
 * Keep the host Multiboot2 fixture in initialized data, before the large
 * allocator bitmaps in BSS. Its address must remain inside the first 8 MiB,
 * which the allocator reserves unconditionally.
 */
static uint8_t boot_info[232]
    __attribute__((aligned(8), section(".data.boot_info"))) = {1u};

void spinlock_init(struct spinlock *lock)
{
    if (lock)
        __atomic_store_n(&lock->value, 0u, __ATOMIC_RELAXED);
}

void spinlock_acquire(struct spinlock *lock)
{
    if (!lock)
        return;
    while (__atomic_exchange_n(&lock->value, 1u, __ATOMIC_ACQUIRE) != 0u)
        __asm__ volatile("pause" ::: "memory");
}

void spinlock_release(struct spinlock *lock)
{
    if (lock)
        __atomic_store_n(&lock->value, 0u, __ATOMIC_RELEASE);
}

uint32_t spinlock_try_acquire(struct spinlock *lock)
{
    uint32_t expected = 0u;
    return lock && __atomic_compare_exchange_n(&lock->value, &expected, 1u,
        0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

uint32_t spinlock_irqsave_acquire(struct spinlock *lock)
{
    spinlock_acquire(lock);
    return 0u;
}

void spinlock_irqrestore_release(struct spinlock *lock, uint32_t flags)
{
    (void)flags;
    spinlock_release(lock);
}

void debug_putc(char c)
{
    (void)c;
}

void debug_write(const char *s)
{
    (void)s;
}

static void put32(uint32_t offset, uint32_t value)
{
    boot_info[offset] = (uint8_t)value;
    boot_info[offset + 1u] = (uint8_t)(value >> 8);
    boot_info[offset + 2u] = (uint8_t)(value >> 16);
    boot_info[offset + 3u] = (uint8_t)(value >> 24);
}

static void put64(uint32_t offset, uint64_t value)
{
    put32(offset, (uint32_t)value);
    put32(offset + 4u, (uint32_t)(value >> 32));
}

static void put_mmap_entry(uint32_t offset, uint64_t start,
                           uint64_t length, uint32_t type)
{
    put64(offset, start);
    put64(offset + 8u, length);
    put32(offset + 16u, type);
    put32(offset + 20u, 0u);
}

static void make_boot_info(void)
{
    for (uint32_t i = 0u; i < sizeof(boot_info); ++i)
        boot_info[i] = 0u;

    /* The standard fixture ends at its end-tag; spare buffer capacity is unused. */
    put32(0u, 200u);
    put32(4u, 0u);

    /* Six entries deliberately put reserved ranges before available ranges. */
    put32(8u, MULTIBOOT2_TAG_MMAP);
    put32(12u, 160u);
    put32(16u, 24u);
    put32(20u, 0u);
    put_mmap_entry(24u, LOW_RESERVED_START,
                   LOW_RESERVED_END - LOW_RESERVED_START, 2u);
    put_mmap_entry(48u, HIGH_RESERVED_START,
                   HIGH_RESERVED_END - HIGH_RESERVED_START, 2u);
    put_mmap_entry(72u, LOW_AVAILABLE_START,
                   LOW_AVAILABLE_END - LOW_AVAILABLE_START,
                   MULTIBOOT2_MEMORY_AVAILABLE);
    put_mmap_entry(96u, LOW_AVAILABLE_START,
                   8ULL * MIB, MULTIBOOT2_MEMORY_AVAILABLE);
    put_mmap_entry(120u, HIGH_AVAILABLE_START,
                   HIGH_AVAILABLE_END - HIGH_AVAILABLE_START,
                   MULTIBOOT2_MEMORY_AVAILABLE);
    put_mmap_entry(144u, HIGH_AVAILABLE_START,
                   4ULL * MIB, MULTIBOOT2_MEMORY_AVAILABLE);

    put32(168u, MULTIBOOT2_TAG_MODULE);
    put32(172u, 17u);
    put32(176u, (uint32_t)MODULE_START);
    put32(180u, (uint32_t)MODULE_END);

    put32(184u, 0u); /* Empty module command line, including its NUL terminator. */
    put32(192u, MULTIBOOT2_TAG_END);
    put32(196u, 8u);
}

static void make_high_module_boot_info(void)
{
    make_boot_info();

    /*
     * Place a valid Multiboot2 module inside the high allocator window.
     * The 32-bit module fields can represent this 516–517 MiB interval.
     */
    put32(176u, 516u * (uint32_t)MIB);
    put32(180u, 517u * (uint32_t)MIB);
}

static void make_multiple_modules_boot_info(void)
{
    make_boot_info();

    /*
     * Insert a second module after the first one. Each module tag has a
     * 17-byte size and therefore occupies 24 bytes after 8-byte alignment.
     */
    put32(0u, 224u);
    put32(192u, MULTIBOOT2_TAG_MODULE);
    put32(196u, 17u);
    put32(200u, 76u * (uint32_t)MIB);
    put32(204u, 77u * (uint32_t)MIB);
    boot_info[208u] = 0u;
    put32(216u, MULTIBOOT2_TAG_END);
    put32(220u, 8u);
}

static void make_available_before_reserved_boot_info(void)
{
    for (uint32_t i = 0u; i < sizeof(boot_info); ++i)
        boot_info[i] = 0u;

    put32(0u, 128u);
    put32(4u, 0u);
    put32(8u, MULTIBOOT2_TAG_MMAP);
    put32(12u, 112u);
    put32(16u, 24u);
    put32(20u, 0u);

    /*
     * Available RAM deliberately precedes overlapping reserved ranges in
     * both allocator windows, not only the low-memory bitmap.
     */
    put_mmap_entry(24u, 16ULL * MIB, 4ULL * MIB,
                   MULTIBOOT2_MEMORY_AVAILABLE);
    put_mmap_entry(48u, 17ULL * MIB, 1ULL * MIB, 2u);
    put_mmap_entry(72u, 512ULL * MIB, 4ULL * MIB,
                   MULTIBOOT2_MEMORY_AVAILABLE);
    put_mmap_entry(96u, 513ULL * MIB, 1ULL * MIB, 2u);
    put32(120u, MULTIBOOT2_TAG_END);
    put32(124u, 8u);
}

static void make_no_usable_memory_boot_info(void)
{
    for (uint32_t i = 0u; i < sizeof(boot_info); ++i)
        boot_info[i] = 0u;

    put32(0u, 56u);
    put32(4u, 0u);
    put32(8u, MULTIBOOT2_TAG_MMAP);
    put32(12u, 40u);
    put32(16u, 24u);
    put32(20u, 0u);
    put_mmap_entry(24u, 1ULL * MIB, 16ULL * MIB, 2u);
    put32(48u, MULTIBOOT2_TAG_END);
    put32(52u, 8u);
}

static void make_boundary_boot_info(void)
{
    for (uint32_t i = 0u; i < sizeof(boot_info); ++i)
        boot_info[i] = 0u;

    put32(0u, 80u);
    put32(4u, 0u);
    put32(8u, MULTIBOOT2_TAG_MMAP);
    put32(12u, 64u);
    put32(16u, 24u);
    put32(20u, 0u);

    /* One range crosses the 256 MiB low/high allocator seam. */
    put_mmap_entry(24u, LOW_MEMORY_LIMIT - PAGE_SIZE,
                   2u * PAGE_SIZE, MULTIBOOT2_MEMORY_AVAILABLE);

    /* The second range crosses the exclusive 64 GiB high-map limit. */
    put_mmap_entry(48u, HIGH_MEMORY_LIMIT - PAGE_SIZE,
                   2u * PAGE_SIZE, MULTIBOOT2_MEMORY_AVAILABLE);

    put32(72u, MULTIBOOT2_TAG_END);
    put32(76u, 8u);
}

static void make_full_high_map_boot_info(void)
{
    for (uint32_t i = 0u; i < sizeof(boot_info); ++i)
        boot_info[i] = 0u;

    put32(0u, 56u);
    put32(4u, 0u);
    put32(8u, MULTIBOOT2_TAG_MMAP);
    put32(12u, 40u);
    put32(16u, 24u);
    put32(20u, 0u);
    put_mmap_entry(24u, LOW_MEMORY_LIMIT,
                   HIGH_MEMORY_LIMIT - LOW_MEMORY_LIMIT,
                   MULTIBOOT2_MEMORY_AVAILABLE);
    put32(48u, MULTIBOOT2_TAG_END);
    put32(52u, 8u);
}

static void make_full_low_map_boot_info(void)
{
    for (uint32_t i = 0u; i < sizeof(boot_info); ++i)
        boot_info[i] = 0u;

    put32(0u, 56u);
    put32(4u, 0u);
    put32(8u, MULTIBOOT2_TAG_MMAP);
    put32(12u, 40u);
    put32(16u, 24u);
    put32(20u, 0u);
    put_mmap_entry(24u, 0u, LOW_MEMORY_LIMIT,
                   MULTIBOOT2_MEMORY_AVAILABLE);
    put32(48u, MULTIBOOT2_TAG_END);
    put32(52u, 8u);
}

static void make_extended_stride_boot_info(void)
{
    for (uint32_t i = 0u; i < sizeof(boot_info); ++i)
        boot_info[i] = 0u;

    put32(0u, 96u);
    put32(4u, 0u);
    put32(8u, MULTIBOOT2_TAG_MMAP);
    put32(12u, 80u);
    put32(16u, 32u);
    put32(20u, 0u);
    put_mmap_entry(24u, 16ULL * MIB, 4ULL * MIB,
                   MULTIBOOT2_MEMORY_AVAILABLE);
    /* The second 32-byte record reserves a 1 MiB interior hole. */
    put_mmap_entry(56u, 17ULL * MIB, 1ULL * MIB, 2u);
    /* Unknown per-entry extension bytes must not affect the base fields. */
    put32(48u, 0xA5A5A5A5u);
    put32(52u, 0x5A5A5A5Au);
    put32(80u, 0x11223344u);
    put32(84u, 0x55667788u);
    put32(88u, MULTIBOOT2_TAG_END);
    put32(92u, 8u);
}

static void make_full_managed_map_boot_info(void)
{
    for (uint32_t i = 0u; i < sizeof(boot_info); ++i)
        boot_info[i] = 0u;

    /*
     * Describe both allocator windows in one firmware map. The first range
     * covers 0–256 MiB and the second covers 256 MiB–64 GiB.
     */
    put32(0u, 80u);
    put32(4u, 0u);
    put32(8u, MULTIBOOT2_TAG_MMAP);
    put32(12u, 64u);
    put32(16u, 24u);
    put32(20u, 0u);
    put_mmap_entry(24u, 0u, LOW_MEMORY_LIMIT,
                   MULTIBOOT2_MEMORY_AVAILABLE);
    put_mmap_entry(48u, LOW_MEMORY_LIMIT,
                   HIGH_MEMORY_LIMIT - LOW_MEMORY_LIMIT,
                   MULTIBOOT2_MEMORY_AVAILABLE);
    put32(72u, MULTIBOOT2_TAG_END);
    put32(76u, 8u);
}


static int fail(int code, const char *message)
{
    printf("physical allocator test failed (%d): %s\n", code, message);
    return code;
}


static volatile uint32_t low_owners[LOW_OWNER_SLOTS];
static volatile uint32_t high_owners[HIGH_OWNER_SLOTS];
static volatile uint32_t memory_stress_failed;

struct memory_stress_argument { uint32_t worker; };

static void memory_stress_fail(void)
{
    __atomic_store_n(&memory_stress_failed, 1u, __ATOMIC_RELEASE);
}

static void *memory_stress_worker(void *opaque)
{
    const struct memory_stress_argument *argument =
        (const struct memory_stress_argument *)opaque;
    const uint32_t owner = argument->worker + 1u;

    for (uint32_t round = 0u; round < MEMORY_STRESS_ITERATIONS; ++round) {
        void *low_pages[MEMORY_STRESS_HELD_PAGES];
        uint64_t high_pages[MEMORY_STRESS_HELD_PAGES];
        uint32_t low_slots[MEMORY_STRESS_HELD_PAGES];
        uint32_t high_slots[MEMORY_STRESS_HELD_PAGES];
        uint8_t low_registered[MEMORY_STRESS_HELD_PAGES] = {0u};
        uint8_t high_registered[MEMORY_STRESS_HELD_PAGES] = {0u};
        uint32_t low_count = 0u;
        uint32_t high_count = 0u;
        int failed = 0;

        for (uint32_t i = 0u; i < MEMORY_STRESS_HELD_PAGES; ++i) {
            low_pages[i] = page_alloc();
            if (!low_pages[i]) { failed = 1; break; }
            ++low_count;

            uintptr_t address = (uintptr_t)low_pages[i];
            if (address < LOW_AVAILABLE_START || address >= LOW_AVAILABLE_END ||
                (address >= LOW_RESERVED_START && address < LOW_RESERVED_END) ||
                (address >= MODULE_START && address < MODULE_END) ||
                ((address - LOW_AVAILABLE_START) % PAGE_SIZE) != 0u) {
                failed = 1;
                break;
            }
            low_slots[i] =
                (uint32_t)((address - LOW_AVAILABLE_START) / PAGE_SIZE);
            uint32_t expected = 0u;
            if (!__atomic_compare_exchange_n(&low_owners[low_slots[i]],
                    &expected, owner, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                failed = 1;
                break;
            }
            low_registered[i] = 1u;

            if (page_alloc_high(&high_pages[i]) != 0) {
                failed = 1;
                break;
            }
            ++high_count;
            address = (uintptr_t)high_pages[i];
            if (high_pages[i] < HIGH_AVAILABLE_START ||
                high_pages[i] >= HIGH_AVAILABLE_END ||
                (high_pages[i] >= HIGH_RESERVED_START &&
                 high_pages[i] < HIGH_RESERVED_END) ||
                ((high_pages[i] - HIGH_AVAILABLE_START) % PAGE_SIZE) != 0u) {
                failed = 1;
                break;
            }
            high_slots[i] =
                (uint32_t)((high_pages[i] - HIGH_AVAILABLE_START) / PAGE_SIZE);
            expected = 0u;
            if (!__atomic_compare_exchange_n(&high_owners[high_slots[i]],
                    &expected, owner, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                failed = 1;
                break;
            }
            high_registered[i] = 1u;
            if (memory_high_page_is_allocated(high_pages[i]) != 1) {
                failed = 1;
                break;
            }
        }

        if (failed) memory_stress_fail();
        else {
            for (volatile uint32_t spin = 0u; spin < 256u; ++spin)
                __asm__ volatile("pause" ::: "memory");
        }

        for (uint32_t i = 0u; i < low_count; ++i) {
            if (low_registered[i]) {
                __atomic_store_n(&low_owners[low_slots[i]], 0u, __ATOMIC_RELEASE);
                page_free(low_pages[i]);
            }
        }
        for (uint32_t i = 0u; i < high_count; ++i) {
            if (high_registered[i]) {
                __atomic_store_n(&high_owners[high_slots[i]], 0u, __ATOMIC_RELEASE);
                page_free_high(high_pages[i]);
            }
        }
        if (failed) return 0;
    }
    return 0;
}

#define HIGH_FRAGMENTATION_SLOTS 64u
#define HIGH_FRAGMENTATION_OPERATIONS 2048u

struct high_fragmentation_run {
    uint64_t address;
    uint32_t count;
};

static uint32_t high_fragmentation_random(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

/*
 * Deterministically fragment the high allocator with varied contiguous runs.
 * Every transition checks page ownership and exact free-page accounting;
 * failed allocations must preserve both the output sentinel and inventory.
 */
static int memory_high_fragmentation_test(uint32_t inventory)
{
    struct high_fragmentation_run runs[HIGH_FRAGMENTATION_SLOTS] = {{0u, 0u}};
    uint32_t random_state = 0x6D2B79F5u;
    uint32_t expected_free = inventory;

    if (memory_high_free_pages() != inventory)
        return -1;

    for (uint32_t operation = 0u;
         operation < HIGH_FRAGMENTATION_OPERATIONS; ++operation) {
        uint32_t random = high_fragmentation_random(&random_state);
        uint32_t slot = random % HIGH_FRAGMENTATION_SLOTS;

        if (runs[slot].count != 0u) {
            uint64_t address = runs[slot].address;
            uint32_t count = runs[slot].count;

            for (uint32_t page = 0u; page < count; ++page) {
                if (memory_high_page_is_allocated(
                        address + (uint64_t)page * PAGE_SIZE) != 1)
                    return -1;
            }

            page_free_high_contiguous(address, count);
            expected_free += count;
            runs[slot].address = 0u;
            runs[slot].count = 0u;

            if (memory_high_free_pages() != expected_free)
                return -1;
            for (uint32_t page = 0u; page < count; ++page) {
                if (memory_high_page_is_allocated(
                        address + (uint64_t)page * PAGE_SIZE) != 0)
                    return -1;
            }
        } else {
            uint32_t count = ((random >> 8) % 32u) + 1u;
            uint64_t address = 0xA7A7A7A7A7A7A7A7ULL;
            uint32_t before = expected_free;

            if (page_alloc_high_contiguous(count, &address) != 0) {
                if (address != 0xA7A7A7A7A7A7A7A7ULL ||
                    memory_high_free_pages() != before)
                    return -1;
                continue;
            }

            if (count > before)
                return -1;
            runs[slot].address = address;
            runs[slot].count = count;
            if (address < LOW_MEMORY_LIMIT ||
                address >= HIGH_MEMORY_LIMIT ||
                (address & (PAGE_SIZE - 1u)) != 0u ||
                (uint64_t)count >
                    (HIGH_MEMORY_LIMIT - address) / PAGE_SIZE)
                return -1;

            uint64_t end = address + (uint64_t)count * PAGE_SIZE;
            for (uint32_t other = 0u;
                 other < HIGH_FRAGMENTATION_SLOTS; ++other) {
                if (other == slot || runs[other].count == 0u)
                    continue;
                uint64_t other_start = runs[other].address;
                uint64_t other_end = other_start +
                    (uint64_t)runs[other].count * PAGE_SIZE;
                if (address < other_end && other_start < end)
                    return -1;
            }

            for (uint32_t page = 0u; page < count; ++page) {
                if (memory_high_page_is_allocated(
                        address + (uint64_t)page * PAGE_SIZE) != 1)
                    return -1;
            }

            expected_free -= count;
            if (memory_high_free_pages() != expected_free)
                return -1;
        }
    }

    for (uint32_t slot = 0u; slot < HIGH_FRAGMENTATION_SLOTS; ++slot) {
        if (runs[slot].count == 0u)
            continue;
        uint64_t address = runs[slot].address;
        uint32_t count = runs[slot].count;
        page_free_high_contiguous(address, count);
        expected_free += count;
        runs[slot].address = 0u;
        runs[slot].count = 0u;
        for (uint32_t page = 0u; page < count; ++page) {
            if (memory_high_page_is_allocated(
                    address + (uint64_t)page * PAGE_SIZE) != 0)
                return -1;
        }
    }

    return expected_free == inventory &&
           memory_high_free_pages() == inventory ? 0 : -1;
}

static int memory_allocator_concurrency_test(void)
{
    pthread_t threads[MEMORY_STRESS_THREADS];
    struct memory_stress_argument arguments[MEMORY_STRESS_THREADS];
    uint32_t created = 0u;
    uint32_t low_before = memory_free_pages();
    uint32_t high_before = memory_high_free_pages();

    __atomic_store_n(&memory_stress_failed, 0u, __ATOMIC_RELAXED);
    for (uint32_t i = 0u; i < LOW_OWNER_SLOTS; ++i)
        __atomic_store_n(&low_owners[i], 0u, __ATOMIC_RELAXED);
    for (uint32_t i = 0u; i < HIGH_OWNER_SLOTS; ++i)
        __atomic_store_n(&high_owners[i], 0u, __ATOMIC_RELAXED);

    for (uint32_t i = 0u; i < MEMORY_STRESS_THREADS; ++i) {
        arguments[i].worker = i;
        if (pthread_create(&threads[i], 0, memory_stress_worker,
                           &arguments[i]) != 0) {
            memory_stress_fail();
            break;
        }
        ++created;
    }
    for (uint32_t i = 0u; i < created; ++i)
        if (pthread_join(threads[i], 0) != 0) memory_stress_fail();

    if (created != MEMORY_STRESS_THREADS ||
        __atomic_load_n(&memory_stress_failed, __ATOMIC_ACQUIRE) != 0u ||
        memory_free_pages() != low_before ||
        memory_high_free_pages() != high_before)
        return -1;

    for (uint32_t i = 0u; i < LOW_OWNER_SLOTS; ++i)
        if (__atomic_load_n(&low_owners[i], __ATOMIC_ACQUIRE) != 0u) return -1;
    for (uint32_t i = 0u; i < HIGH_OWNER_SLOTS; ++i)
        if (__atomic_load_n(&high_owners[i], __ATOMIC_ACQUIRE) != 0u) return -1;
    return 0;
}

static int expect_rejected_boot_info(int code, const char *message)
{
    if (memory_init((uint32_t)(uintptr_t)boot_info) == 0)
        return fail(code, message);
    if (page_alloc() != 0 || memory_free_pages() != 0u ||
        memory_high_free_pages() != 0u || memory_total_pages() != 0u ||
        memory_high_available_bytes() != 0u)
        return fail(code + 1, "rejected boot metadata left allocator pages exposed");
    return 0;
}


struct high_pin_race_context {
    uint64_t physical_address;
    volatile uint32_t free_completed;
    volatile uint32_t failed;
};

static void *high_pin_race_free_worker(void *opaque)
{
    struct high_pin_race_context *context =
        (struct high_pin_race_context *)opaque;

    page_free_high(context->physical_address);
    __atomic_store_n(&context->free_completed, 1u, __ATOMIC_RELEASE);
    return 0;
}

static void *high_pin_race_allocate_worker(void *opaque)
{
    struct high_pin_race_context *context =
        (struct high_pin_race_context *)opaque;
    uint64_t allocation = 0u;

    while (__atomic_load_n(&context->free_completed, __ATOMIC_ACQUIRE) == 0u)
        __asm__ volatile("pause" ::: "memory");

    /* A release-pending page must reject new pins and remain unavailable. */
    if (memory_high_page_pin(context->physical_address) != -1)
        __atomic_store_n(&context->failed, 1u, __ATOMIC_RELEASE);

    if (page_alloc_high(&allocation) != 0) {
        __atomic_store_n(&context->failed, 1u, __ATOMIC_RELEASE);
    } else {
        if (allocation == context->physical_address)
            __atomic_store_n(&context->failed, 1u, __ATOMIC_RELEASE);
        page_free_high(allocation);
    }

    if (memory_high_page_unpin(context->physical_address) != 0)
        __atomic_store_n(&context->failed, 1u, __ATOMIC_RELEASE);
    return 0;
}

static int high_pin_concurrent_release_test(uint32_t inventory)
{
    struct high_pin_race_context context = {0u, 0u, 0u};
    pthread_t free_thread;
    pthread_t allocate_thread;

    if (page_alloc_high(&context.physical_address) != 0)
        return -1;
    if (memory_high_page_pin(context.physical_address) != 0) {
        page_free_high(context.physical_address);
        return -1;
    }

    if (pthread_create(&free_thread, 0, high_pin_race_free_worker,
                       &context) != 0) {
        (void)memory_high_page_unpin(context.physical_address);
        page_free_high(context.physical_address);
        return -1;
    }

    if (pthread_create(&allocate_thread, 0, high_pin_race_allocate_worker,
                       &context) != 0) {
        (void)pthread_join(free_thread, 0);
        (void)memory_high_page_unpin(context.physical_address);
        return -1;
    }

    if (pthread_join(free_thread, 0) != 0)
        __atomic_store_n(&context.failed, 1u, __ATOMIC_RELEASE);
    if (pthread_join(allocate_thread, 0) != 0)
        __atomic_store_n(&context.failed, 1u, __ATOMIC_RELEASE);

    if (__atomic_load_n(&context.failed, __ATOMIC_ACQUIRE) != 0u ||
        memory_high_page_is_allocated(context.physical_address) != 0 ||
        memory_high_free_pages() != inventory)
        return -1;
    return 0;
}


static int high_pin_capacity_test(uint32_t inventory)
{
    const uint32_t pin_count = 8192u;
    const uint32_t allocation_count = pin_count + 1u;
    uint64_t base = 0u;

    if (page_alloc_high_contiguous(allocation_count, &base) != 0)
        return -1;

    for (uint32_t i = 0u; i < pin_count; ++i) {
        uint64_t address = base + (uint64_t)i * PAGE_SIZE;
        if (memory_high_page_pin(address) != 0) {
            for (uint32_t j = 0u; j < i; ++j)
                (void)memory_high_page_unpin(base + (uint64_t)j * PAGE_SIZE);
            page_free_high_contiguous(base, allocation_count);
            return -1;
        }
    }

    /* The bounded metadata table must fail closed without corrupting pins. */
    if (memory_high_page_pin(base + (uint64_t)pin_count * PAGE_SIZE) == 0) {
        (void)memory_high_page_unpin(base + (uint64_t)pin_count * PAGE_SIZE);
        for (uint32_t i = 0u; i < pin_count; ++i)
            (void)memory_high_page_unpin(base + (uint64_t)i * PAGE_SIZE);
        page_free_high_contiguous(base, allocation_count);
        return -1;
    }

    for (uint32_t i = pin_count; i > 0u; --i) {
        if (memory_high_page_unpin(base + (uint64_t)(i - 1u) * PAGE_SIZE) != 0) {
            /* Preserve the allocation rather than freeing still-pinned pages. */
            return -1;
        }
    }

    page_free_high_contiguous(base, allocation_count);
    if (memory_high_free_pages() != inventory)
        return -1;
    return 0;
}

int main(void)
{
    uint64_t high_address = 0u;
    void *low_allocation;

    if (((uintptr_t)boot_info & 7u) != 0u ||
        (uintptr_t)boot_info > UINT32_MAX)
        return fail(1, "Multiboot2 test buffer is not addressable by the 32-bit API");

    make_boot_info();
    if (multiboot2_info_validate(boot_info) != 0)
        return fail(2, "test Multiboot2 information is invalid");

    if (memory_init((uint32_t)(uintptr_t)boot_info) != 0)
        return fail(49, "valid Multiboot2 map failed allocator initialization");

    if (memory_total_pages() !=
        (uint32_t)(LOW_AVAILABLE_END / PAGE_SIZE))
        return fail(3, "low-memory managed-page ceiling is incorrect");

    if (memory_managed_limit_bytes() != HIGH_MEMORY_LIMIT)
        return fail(42, "managed physical-address limit does not include the high-memory window");

    if (memory_free_pages() != 2816u)
        return fail(4, "low-memory count did not deduplicate and reserve overlaps");

    if (memory_high_available_bytes() != 8ULL * MIB)
        return fail(5, "high-memory inventory did not deduplicate overlapping ranges");

    if (memory_high_free_pages() != 1792u)
        return fail(6, "high-memory free count did not apply reserved ranges");

    /*
     * Every module record must reserve its own physical interval. The
     * allocations below walk up to, between, and beyond two module holes.
     */
    make_multiple_modules_boot_info();
    if (multiboot2_info_validate(boot_info) != 0 ||
        memory_init((uint32_t)(uintptr_t)boot_info) != 0 ||
        memory_free_pages() != 2560u)
        return fail(105, "multiple module records were not ingested and reserved");

    void *module_test_low = page_alloc_contiguous(1024u);
    if ((uintptr_t)module_test_low != LOW_AVAILABLE_START)
        return fail(106, "allocator did not stop at the firmware-reserved interval");
    module_test_low = page_alloc_contiguous(512u);
    if ((uintptr_t)module_test_low != 72ULL * MIB)
        return fail(107, "allocator did not skip the firmware-reserved interval");
    module_test_low = page_alloc_contiguous(256u);
    if ((uintptr_t)module_test_low != 75ULL * MIB)
        return fail(108, "allocator did not skip the first module interval");
    module_test_low = page_alloc();
    if ((uintptr_t)module_test_low != 77ULL * MIB)
        return fail(109, "allocator exposed pages from the second module interval");

    /* Restore the standard fixture before the remaining allocator regressions. */
    make_boot_info();
    if (memory_init((uint32_t)(uintptr_t)boot_info) != 0)
        return fail(110, "standard memory fixture failed after module coverage");

    if (page_refcount((void *)(uintptr_t)LOW_RESERVED_START) != 0u)
        return fail(7, "reserved low-memory page unexpectedly has a reference");

    low_allocation = page_alloc_contiguous(3u);
    if ((uintptr_t)low_allocation != LOW_AVAILABLE_START)
        return fail(8, "contiguous low-page allocation returned an unexpected base");

    if (memory_free_pages() != 2813u ||
        page_refcount(low_allocation) != 1u ||
        page_refcount((void *)((uintptr_t)low_allocation + PAGE_SIZE)) != 1u ||
        page_refcount((void *)((uintptr_t)low_allocation + 2u * PAGE_SIZE)) != 1u)
        return fail(9, "low-page allocation accounting or initial references are incorrect");

    page_retain(low_allocation);
    if (page_refcount(low_allocation) != 2u)
        return fail(10, "page_retain did not increment the reference count");

    page_free(low_allocation);
    if (page_refcount(low_allocation) != 1u)
        return fail(11, "page_free did not decrement a retained page");

    page_free((void *)((uintptr_t)low_allocation + PAGE_SIZE));
    page_free((void *)((uintptr_t)low_allocation + 2u * PAGE_SIZE));
    page_free(low_allocation);
    if (memory_free_pages() != 2816u ||
        page_refcount(low_allocation) != 0u)
        return fail(12, "low-page release or double-free protection is incorrect");

    /*
     * Reference counts must saturate instead of wrapping to zero and
     * accidentally returning a still-owned page to the free allocator.
     */
    low_allocation = page_alloc();
    if ((uintptr_t)low_allocation != LOW_AVAILABLE_START)
        return fail(111, "could not allocate a page for reference saturation coverage");
    for (uint32_t i = 1u; i < UINT16_MAX; ++i)
        page_retain(low_allocation);
    page_retain(low_allocation);
    page_retain((void *)(uintptr_t)(LOW_AVAILABLE_START + 1u));
    page_retain((void *)(uintptr_t)LOW_RESERVED_START);
    if (page_refcount(low_allocation) != UINT16_MAX ||
        page_refcount((void *)(uintptr_t)(LOW_AVAILABLE_START + 1u)) != 0u ||
        page_refcount((void *)(uintptr_t)LOW_RESERVED_START) != 0u ||
        memory_free_pages() != 2815u)
        return fail(112, "low-page reference saturation or invalid retain handling failed");

    page_free(low_allocation);
    if (page_refcount(low_allocation) != UINT16_MAX - 1u ||
        memory_free_pages() != 2815u)
        return fail(113, "saturated low-page reference count wrapped or freed early");
    for (uint32_t i = 0u; i < UINT16_MAX - 1u; ++i)
        page_free(low_allocation);
    page_free(low_allocation);
    page_free((void *)(uintptr_t)(LOW_AVAILABLE_START + 1u));
    page_free((void *)(uintptr_t)LOW_RESERVED_START);
    if (page_refcount(low_allocation) != 0u ||
        memory_free_pages() != 2816u)
        return fail(114, "saturated or invalid low-page releases corrupted accounting");

    /*
     * A runtime reservation must revoke an allocated low page, and a
     * zero-reference reserved page must not be resurrected by page_retain.
     */
    low_allocation = page_alloc();
    if ((uintptr_t)low_allocation != LOW_AVAILABLE_START)
        return fail(43, "low allocator did not reuse the first released page");
    uint32_t low_free_before_reserve = memory_free_pages();
    memory_reserve_physical_range((uint64_t)(uintptr_t)low_allocation,
                                  (uint64_t)(uintptr_t)low_allocation + PAGE_SIZE);
    /* Repeating a reservation of an unavailable low page must be harmless. */
    memory_reserve_physical_range((uint64_t)(uintptr_t)low_allocation,
                                  (uint64_t)(uintptr_t)low_allocation + PAGE_SIZE);
    if (memory_free_pages() != low_free_before_reserve ||
        page_refcount(low_allocation) != 0u)
        return fail(44, "reserving an allocated low page did not revoke its reference");
    page_retain(low_allocation);
    if (page_refcount(low_allocation) != 0u)
        return fail(45, "page_retain resurrected a reserved low page");
    page_free(low_allocation);
    if (memory_free_pages() != low_free_before_reserve)
        return fail(46, "freeing a revoked low page changed allocator accounting");

    low_allocation = page_alloc();
    if ((uintptr_t)low_allocation != LOW_AVAILABLE_START + PAGE_SIZE)
        return fail(47, "low allocator returned a page that was reserved at runtime");
    page_free(low_allocation);
    if (memory_free_pages() != low_free_before_reserve)
        return fail(48, "low-page allocation after reservation did not release cleanly");

    if (page_alloc_high(&high_address) != 0 ||
        high_address != HIGH_AVAILABLE_START)
        return fail(13, "high-page allocator did not return the first available page");

    if (memory_high_free_pages() != 1791u ||
        memory_range_contains_available_ram(HIGH_RESERVED_START,
                                            HIGH_RESERVED_END) != 0 ||
        memory_range_contains_available_ram(HIGH_AVAILABLE_START,
                                            HIGH_AVAILABLE_START + PAGE_SIZE) != 1 ||
        memory_high_page_is_allocated(high_address) != 1 ||
        memory_high_page_is_allocated(HIGH_AVAILABLE_START + PAGE_SIZE) != 0 ||
        memory_high_page_is_allocated(HIGH_RESERVED_START) != 0 ||
        memory_high_page_is_allocated(HIGH_AVAILABLE_START + 1u) != -1 ||
        memory_high_page_is_allocated(LOW_MEMORY_LIMIT - PAGE_SIZE) != -1 ||
        memory_high_page_is_allocated(HIGH_MEMORY_LIMIT) != -1)
        return fail(14, "high-page ownership query or allocation accounting failed");

    page_free_high(high_address);
    if (memory_high_free_pages() != 1792u ||
        memory_high_page_is_allocated(high_address) != 0)
        return fail(15, "high-page release did not clear ownership or restore the free count");

    /*
     * Failed high contiguous allocations must be transactional: reject zero
     * pages and requests larger than the current free inventory without
     * changing the caller's output address or allocator accounting.
     */
    high_address = 0x1122334455667788ULL;
    if (page_alloc_high_contiguous(0u, &high_address) == 0 ||
        high_address != 0x1122334455667788ULL ||
        memory_high_free_pages() != 1792u)
        return fail(90, "zero-length high allocation changed output or accounting");

    if (page_alloc_high_contiguous(1793u, &high_address) == 0 ||
        high_address != 0x1122334455667788ULL ||
        memory_high_free_pages() != 1792u)
        return fail(91, "oversized high allocation changed output or accounting");

    memory_reserve_physical_range(516ULL * MIB, 517ULL * MIB);
    if (memory_high_free_pages() != 1536u ||
        memory_range_contains_available_ram(516ULL * MIB,
                                            517ULL * MIB) != 0 ||
        memory_high_available_bytes() != 8ULL * MIB)
        return fail(16, "runtime high-memory reservation changed the wrong accounting");

    /*
     * Reserving an already allocated high page must revoke that page without
     * double-decrementing the free count. Releasing the original run must
     * return only the two pages that remain firmware-available.
     */
    if (page_alloc_high_contiguous(3u, &high_address) != 0 ||
        high_address != HIGH_AVAILABLE_START ||
        memory_high_free_pages() != 1533u ||
        memory_high_page_is_allocated(high_address) != 1 ||
        memory_high_page_is_allocated(high_address + PAGE_SIZE) != 1 ||
        memory_high_page_is_allocated(high_address + 2u * PAGE_SIZE) != 1)
        return fail(100, "high allocator could not prepare an owned contiguous reservation run");

    const uint64_t revoked_high_page = HIGH_AVAILABLE_START + PAGE_SIZE;
    memory_reserve_physical_range(revoked_high_page,
                                  revoked_high_page + PAGE_SIZE);
    /* Reapplying the same reservation must be strictly idempotent. */
    memory_reserve_physical_range(revoked_high_page,
                                  revoked_high_page + PAGE_SIZE);
    if (memory_high_free_pages() != 1533u ||
        memory_range_contains_available_ram(HIGH_AVAILABLE_START,
                                            HIGH_AVAILABLE_START + PAGE_SIZE) != 1 ||
        memory_range_contains_available_ram(revoked_high_page,
                                            revoked_high_page + PAGE_SIZE) != 0 ||
        memory_high_page_is_allocated(revoked_high_page) != 0 ||
        memory_high_page_is_allocated(high_address) != 1 ||
        memory_high_page_is_allocated(high_address + 2u * PAGE_SIZE) != 1 ||
        memory_range_contains_available_ram(HIGH_AVAILABLE_START + 2u * PAGE_SIZE,
                                            HIGH_AVAILABLE_START + 3u * PAGE_SIZE) != 1)
        return fail(101, "reserving an allocated high page corrupted availability or accounting");

    page_free_high_contiguous(high_address, 3u);
    if (memory_high_free_pages() != 1535u ||
        memory_high_page_is_allocated(high_address) != 0 ||
        memory_high_page_is_allocated(revoked_high_page) != 0 ||
        memory_high_page_is_allocated(high_address + 2u * PAGE_SIZE) != 0)
        return fail(102, "releasing a run with a revoked high page restored incorrect ownership or free count");

    uint64_t first_remaining_page = 0u;
    uint64_t second_remaining_page = 0u;
    if (page_alloc_high(&first_remaining_page) != 0 ||
        page_alloc_high(&second_remaining_page) != 0 ||
        first_remaining_page != HIGH_AVAILABLE_START ||
        second_remaining_page != HIGH_AVAILABLE_START + 2u * PAGE_SIZE ||
        memory_high_free_pages() != 1533u)
        return fail(103, "revoked high page was reused or neighboring pages were lost");

    page_free_high(first_remaining_page);
    page_free_high(second_remaining_page);
    if (memory_high_free_pages() != 1535u)
        return fail(104, "high-page release after runtime revocation did not restore accounting");

    if (page_alloc_high(&high_address) != 0 ||
        high_address != HIGH_AVAILABLE_START)
        return fail(17, "high-page allocator did not preserve the scan after reservation");
    page_free_high(high_address);

    /*
     * Exercise both sides of the low/high allocator seam and the final page
     * below the high allocator's exclusive 64 GiB limit.
     */
    make_boundary_boot_info();
    if (memory_init((uint32_t)(uintptr_t)boot_info) != 0)
        return fail(50, "boundary map failed allocator initialization");

    if (memory_total_pages() != LOW_MEMORY_LIMIT / PAGE_SIZE ||
        memory_free_pages() != 1u ||
        memory_high_free_pages() != 2u ||
        memory_high_available_bytes() != 2u * PAGE_SIZE)
        return fail(18, "low/high seam or upper-bound inventory was clipped incorrectly");

    low_allocation = page_alloc();
    if ((uintptr_t)low_allocation != LOW_MEMORY_LIMIT - PAGE_SIZE)
        return fail(19, "low allocator did not expose the final page below 256 MiB");

    if (page_alloc_high(&high_address) != 0 ||
        high_address != LOW_MEMORY_LIMIT)
        return fail(20, "high allocator did not expose the first page at 256 MiB");

    if (page_alloc_high(&high_address) != 0 ||
        high_address != HIGH_MEMORY_LIMIT - PAGE_SIZE)
        return fail(21, "high allocator did not expose the final page below 64 GiB");

    if (memory_range_contains_available_ram(HIGH_MEMORY_LIMIT - PAGE_SIZE,
                                            HIGH_MEMORY_LIMIT) != 1 ||
        memory_range_contains_available_ram(HIGH_MEMORY_LIMIT,
                                            HIGH_MEMORY_LIMIT + PAGE_SIZE) != -1)
        return fail(22, "high-memory range query mishandled the exclusive upper bound");

    page_free(low_allocation);
    page_free_high(LOW_MEMORY_LIMIT);
    page_free_high(HIGH_MEMORY_LIMIT - PAGE_SIZE);
    if (memory_free_pages() != 1u || memory_high_free_pages() != 2u)
        return fail(23, "boundary-page release did not restore allocator counts");

    /*
     * Invalid high-page releases must be no-ops, including a run crossing
     * the exclusive 64 GiB limit and ranges that are unaligned, empty,
     * outside the managed window, or not currently allocated.
     */
    page_free_high_contiguous(HIGH_MEMORY_LIMIT - PAGE_SIZE, 2u);
    page_free_high_contiguous(LOW_MEMORY_LIMIT + 1u, 1u);
    page_free_high_contiguous(LOW_MEMORY_LIMIT, 0u);
    page_free_high_contiguous(LOW_MEMORY_LIMIT, 1u);
    page_free_high_contiguous(HIGH_MEMORY_LIMIT, 1u);
    if (memory_free_pages() != 1u ||
        memory_high_free_pages() != 2u ||
        memory_high_available_bytes() != 2u * PAGE_SIZE)
        return fail(115, "invalid or unallocated high-page release changed allocator accounting");

    memory_reserve_physical_range(HIGH_MEMORY_LIMIT - PAGE_SIZE,
                                  HIGH_MEMORY_LIMIT);
    if (memory_high_free_pages() != 1u ||
        memory_high_available_bytes() != 2u * PAGE_SIZE ||
        memory_range_contains_available_ram(HIGH_MEMORY_LIMIT - PAGE_SIZE,
                                            HIGH_MEMORY_LIMIT) != 0)
        return fail(24, "runtime reservation at the 64 GiB boundary is incorrect");

    /* A byte range straddling 256 MiB must reserve one page in each map. */
    memory_reserve_physical_range(LOW_MEMORY_LIMIT - PAGE_SIZE / 2u,
                                  LOW_MEMORY_LIMIT + PAGE_SIZE / 2u);
    if (memory_free_pages() != 0u || memory_high_free_pages() != 0u ||
        memory_range_contains_available_ram(LOW_MEMORY_LIMIT,
                                            LOW_MEMORY_LIMIT + PAGE_SIZE) != 0)
        return fail(25, "cross-boundary reservation did not reach both allocators");

    /*
     * Populate the complete supported high-memory window, then reserve its
     * interior. This exercises every high-map bitmap page, not only samples.
     */
    make_full_high_map_boot_info();
    if (multiboot2_info_validate(boot_info) != 0)
        return fail(26, "full high-memory map metadata is invalid");
    if (memory_init((uint32_t)(uintptr_t)boot_info) != 0)
        return fail(51, "full high-memory map failed allocator initialization");

    if (memory_high_free_pages() !=
            (uint32_t)((HIGH_MEMORY_LIMIT - LOW_MEMORY_LIMIT) / PAGE_SIZE) ||
        memory_high_available_bytes() != HIGH_MEMORY_LIMIT - LOW_MEMORY_LIMIT ||
        memory_range_contains_available_ram(LOW_MEMORY_LIMIT,
                                            HIGH_MEMORY_LIMIT) != 1)
        return fail(27, "complete high-memory window was not accounted or queryable");

    if (page_alloc_high(&high_address) != 0 ||
        high_address != LOW_MEMORY_LIMIT)
        return fail(28, "full-window allocation did not begin at 256 MiB");

    memory_reserve_physical_range(LOW_MEMORY_LIMIT + PAGE_SIZE,
                                  HIGH_MEMORY_LIMIT - PAGE_SIZE);
    if (memory_high_free_pages() != 1u ||
        memory_range_contains_available_ram(LOW_MEMORY_LIMIT + PAGE_SIZE,
                                            HIGH_MEMORY_LIMIT - PAGE_SIZE) != 0 ||
        memory_range_contains_available_ram(LOW_MEMORY_LIMIT,
                                            HIGH_MEMORY_LIMIT) != 1 ||
        memory_range_contains_available_ram(HIGH_MEMORY_LIMIT - PAGE_SIZE,
                                            HIGH_MEMORY_LIMIT) != 1)
        return fail(29, "full-window reservation did not preserve only the boundary pages");

    /*
     * The range query reports whether ANY page in the requested interval is
     * firmware-available, even when the interval also contains reserved RAM.
     * It rejects unaligned and out-of-window intervals before scanning.
     */
    if (memory_range_contains_available_ram(LOW_MEMORY_LIMIT,
                                            LOW_MEMORY_LIMIT + 2u * PAGE_SIZE) != 1 ||
        memory_range_contains_available_ram(LOW_MEMORY_LIMIT + PAGE_SIZE,
                                            LOW_MEMORY_LIMIT + 2u * PAGE_SIZE) != 0 ||
        memory_range_contains_available_ram(LOW_MEMORY_LIMIT + 1u,
                                            LOW_MEMORY_LIMIT + PAGE_SIZE) != -1 ||
        memory_range_contains_available_ram(LOW_MEMORY_LIMIT - PAGE_SIZE,
                                            LOW_MEMORY_LIMIT) != -1)
        return fail(81, "high-memory range query mishandled mixed, reserved, or invalid intervals");

    high_address = 0xC3C3C3C3C3C3C3C3ULL;
    if (page_alloc_high_contiguous(2u, &high_address) == 0 ||
        high_address != 0xC3C3C3C3C3C3C3C3ULL ||
        memory_high_free_pages() != 1u)
        return fail(64, "high allocation with insufficient free pages modified output or accounting");

    if (page_alloc_high(&high_address) != 0 ||
        high_address != HIGH_MEMORY_LIMIT - PAGE_SIZE ||
        memory_high_free_pages() != 0u)
        return fail(30, "allocator failed to reach the final page after full-window reservation");

    page_free_high(LOW_MEMORY_LIMIT);
    page_free_high(HIGH_MEMORY_LIMIT - PAGE_SIZE);
    if (memory_high_free_pages() != 2u)
        return fail(31, "full-window allocated pages were not returned correctly");

    /*
     * Verify contiguous high-page allocation, fragmentation handling, and
     * reservation of a page that was already allocated.
     */
    make_full_high_map_boot_info();
    if (memory_init((uint32_t)(uintptr_t)boot_info) != 0)
        return fail(52, "contiguous map failed allocator initialization");
    const uint32_t full_high_pages =
        (uint32_t)((HIGH_MEMORY_LIMIT - LOW_MEMORY_LIMIT) / PAGE_SIZE);

    /* Failed high allocations must preserve caller-owned output storage. */
    high_address = 0xA5A5A5A5A5A5A5A5ULL;
    if (page_alloc_high_contiguous(0u, &high_address) == 0 ||
        high_address != 0xA5A5A5A5A5A5A5A5ULL ||
        page_alloc_high_contiguous(3u, 0) == 0)
        return fail(32, "high contiguous allocator accepted invalid arguments or modified output");

    high_address = 0x5A5A5A5A5A5A5A5AULL;
    if (page_alloc_high_contiguous(full_high_pages + 1u, &high_address) == 0 ||
        high_address != 0x5A5A5A5A5A5A5A5AULL ||
        memory_high_free_pages() != full_high_pages)
        return fail(70, "oversized high allocation modified output or allocator accounting");

    /*
     * The entire 256 MiB–64 GiB window must be allocatable as one physically
     * contiguous run, then fully recoverable without losing bitmap state.
     */
    if (page_alloc_high_contiguous(full_high_pages, &high_address) != 0 ||
        high_address != LOW_MEMORY_LIMIT ||
        memory_high_free_pages() != 0u)
        return fail(79, "full high-memory window was not allocated contiguously");
    page_free_high_contiguous(high_address, full_high_pages);
    if (memory_high_free_pages() != full_high_pages)
        return fail(80, "full high-memory contiguous release did not restore all pages");

    if (memory_high_fragmentation_test(full_high_pages) != 0)
        return fail(116, "deterministic high-memory fragmentation stress corrupted ownership or accounting");

    /*
     * The fragmentation stress intentionally perturbs the scan hint. Rebuild
     * the fixture so the following wraparound test starts from a known state.
     */
    make_full_high_map_boot_info();
    if (memory_init((uint32_t)(uintptr_t)boot_info) != 0)
        return fail(117, "high-memory map could not be restored after fragmentation stress");

    /*
     * Drive the high allocator scan hint to the final page, wrap it to zero,
     * then release the first page. The next allocation must wrap/revisit the
     * newly free low page instead of reporting false exhaustion.
     */
    uint64_t wrap_first = 0u;
    uint64_t wrap_middle = 0u;
    uint64_t wrap_last = 0u;
    uint64_t wrap_reused = 0u;
    if (page_alloc_high(&wrap_first) != 0 ||
        wrap_first != LOW_MEMORY_LIMIT ||
        page_alloc_high_contiguous(full_high_pages - 2u, &wrap_middle) != 0 ||
        wrap_middle != LOW_MEMORY_LIMIT + PAGE_SIZE ||
        page_alloc_high(&wrap_last) != 0 ||
        wrap_last != HIGH_MEMORY_LIMIT - PAGE_SIZE ||
        memory_high_free_pages() != 0u)
        return fail(92, "high allocator failed to advance its scan hint to the final page");

    page_free_high(wrap_first);
    if (memory_high_free_pages() != 1u ||
        page_alloc_high(&wrap_reused) != 0 ||
        wrap_reused != wrap_first ||
        memory_high_free_pages() != 0u)
        return fail(93, "high allocator failed to wrap its scan hint and reuse a released first page");

    page_free_high(wrap_reused);
    page_free_high_contiguous(wrap_middle, full_high_pages - 2u);
    page_free_high(wrap_last);
    if (memory_high_free_pages() != full_high_pages)
        return fail(94, "high allocator scan-hint wraparound sequence did not restore all pages");

    if (page_alloc_high_contiguous(3u, &high_address) != 0 ||
        high_address != LOW_MEMORY_LIMIT ||
        memory_high_free_pages() != full_high_pages - 3u)
        return fail(33, "high contiguous allocation or accounting failed");

    /*
     * Partial and overlapping releases must free each allocated page once.
     * A repeated release must not inflate the free-page counter.
     */
    page_free_high_contiguous(high_address, 2u);
    if (memory_high_free_pages() != full_high_pages - 1u)
        return fail(34, "partial high contiguous release freed the wrong page count");
    page_free_high_contiguous(high_address, 3u);
    if (memory_high_free_pages() != full_high_pages)
        return fail(82, "overlapping high contiguous release did not free only remaining pages");
    page_free_high_contiguous(high_address, 3u);
    if (memory_high_free_pages() != full_high_pages)
        return fail(83, "repeated high contiguous release double-counted free pages");

    if (page_alloc_high_contiguous(3u, &high_address) != 0 ||
        high_address != LOW_MEMORY_LIMIT)
        return fail(35, "high contiguous allocator did not reuse the first run");

    memory_reserve_physical_range(LOW_MEMORY_LIMIT + PAGE_SIZE,
                                  LOW_MEMORY_LIMIT + 2u * PAGE_SIZE);
    if (memory_high_free_pages() != full_high_pages - 3u ||
        memory_range_contains_available_ram(LOW_MEMORY_LIMIT + PAGE_SIZE,
                                            LOW_MEMORY_LIMIT + 2u * PAGE_SIZE) != 0)
        return fail(36, "reserving an allocated high page corrupted free-page accounting");

    page_free_high_contiguous(high_address, 3u);
    if (memory_high_free_pages() != full_high_pages - 1u)
        return fail(37, "releasing a run did not preserve its newly reserved page");

    if (page_alloc_high_contiguous(3u, &high_address) != 0 ||
        high_address != LOW_MEMORY_LIMIT + 2u * PAGE_SIZE ||
        memory_high_free_pages() != full_high_pages - 4u)
        return fail(38, "high contiguous allocator crossed a reserved-page hole");

    page_free_high_contiguous(high_address, 3u);
    if (memory_high_free_pages() != full_high_pages - 1u)
        return fail(39, "fragmented contiguous allocation did not release cleanly");

    page_free_high_contiguous(HIGH_MEMORY_LIMIT - PAGE_SIZE, 2u);
    page_free_high_contiguous(LOW_MEMORY_LIMIT + 1u, 1u);
    if (memory_high_free_pages() != full_high_pages - 1u)
        return fail(40, "invalid high contiguous release changed allocator state");

    /* Exercise both allocator windows under real host-thread contention. */
    make_boot_info();
    if (memory_init((uint32_t)(uintptr_t)boot_info) != 0)
        return fail(54, "concurrency map failed allocator initialization");
    if (memory_allocator_concurrency_test() != 0)
        return fail(55, "concurrent low/high allocations duplicated pages or leaked free-page accounting");

    /*
     * Multiboot2 modules may reside above the low allocator's 256 MiB
     * boundary. Their physical interval must be excluded from the high
     * allocator even when it overlaps firmware-available RAM.
     */
    make_high_module_boot_info();
    if (multiboot2_info_validate(boot_info) != 0 ||
        memory_init((uint32_t)(uintptr_t)boot_info) != 0)
        return fail(95, "high-memory module map failed validation or initialization");

    if (memory_high_free_pages() != 1536u ||
        memory_high_available_bytes() != 8ULL * MIB ||
        memory_range_contains_available_ram(516ULL * MIB,
                                            517ULL * MIB) != 0 ||
        memory_range_contains_available_ram(515ULL * MIB,
                                            516ULL * MIB) != 1 ||
        memory_range_contains_available_ram(517ULL * MIB,
                                            518ULL * MIB) != 1)
        return fail(96, "high-memory module reservation corrupted available-RAM inventory");

    high_address = 0xD4D4D4D4D4D4D4D4ULL;
    if (page_alloc_high_contiguous(769u, &high_address) == 0 ||
        high_address != 0xD4D4D4D4D4D4D4D4ULL ||
        memory_high_free_pages() != 1536u)
        return fail(97, "high contiguous allocation crossed a reserved module interval");

    if (page_alloc_high_contiguous(768u, &high_address) != 0 ||
        high_address != 517ULL * MIB ||
        memory_high_free_pages() != 768u)
        return fail(98, "high allocator failed to select the largest run after a module reservation");
    page_free_high_contiguous(high_address, 768u);
    if (memory_high_free_pages() != 1536u)
        return fail(99, "high-memory module reservation test did not restore allocator accounting");

    /*
     * Exercise every page in the complete low-memory bitmap. The first
     * 8 MiB remain reserved; all remaining pages must form one allocatable
     * run and return to the allocator without changing the managed limit.
     */
    make_full_low_map_boot_info();
    if ((uintptr_t)boot_info >= 8ULL * MIB ||
        (uint64_t)(uintptr_t)boot_info + sizeof(boot_info) > 8ULL * MIB)
        return fail(70, "host Multiboot2 fixture escaped the permanently reserved low-memory window");
    if (multiboot2_info_validate(boot_info) != 0 ||
        memory_init((uint32_t)(uintptr_t)boot_info) != 0)
        return fail(71, "complete low-memory map failed allocator initialization");

    const uint32_t full_low_pages =
        (uint32_t)(LOW_MEMORY_LIMIT / PAGE_SIZE) - LOW_RESERVED_PAGES;
    uint32_t full_low_free_before = memory_free_pages();
    void *oversized_low_run = page_alloc_contiguous(full_low_pages + 1u);
    uint32_t full_low_free_after = memory_free_pages();
    uint32_t full_low_total = memory_total_pages();
    if (full_low_total != (uint32_t)(LOW_MEMORY_LIMIT / PAGE_SIZE) ||
        full_low_free_before != full_low_pages ||
        oversized_low_run != 0 ||
        full_low_free_after != full_low_pages) {
        printf("low-memory diagnostic: total=%u expected_total=%u free_before=%u expected_free=%u oversized=%p free_after=%u\\n",
               full_low_total, (uint32_t)(LOW_MEMORY_LIMIT / PAGE_SIZE),
               full_low_free_before, full_low_pages, oversized_low_run,
               full_low_free_after);
        return fail(72, "complete low-memory inventory or oversized allocation handling failed");
    }

    void *full_low_run = page_alloc_contiguous(full_low_pages);
    if ((uintptr_t)full_low_run != 2048ULL * PAGE_SIZE ||
        memory_free_pages() != 0u ||
        page_refcount(full_low_run) != 1u ||
        page_refcount((void *)((uintptr_t)full_low_run +
                      (uint64_t)(full_low_pages - 1u) * PAGE_SIZE)) != 1u ||
        page_alloc() != 0 || memory_free_pages() != 0u)
        return fail(73, "full low-memory allocation did not cover the exact usable range");

    for (uint32_t i = 0u; i < full_low_pages; ++i)
        page_free((void *)((uintptr_t)full_low_run + (uint64_t)i * PAGE_SIZE));

    if (memory_total_pages() != (uint32_t)(LOW_MEMORY_LIMIT / PAGE_SIZE) ||
        memory_free_pages() != full_low_pages ||
        page_refcount(full_low_run) != 0u ||
        page_refcount((void *)((uintptr_t)full_low_run +
                      (uint64_t)(full_low_pages - 1u) * PAGE_SIZE)) != 0u)
        return fail(74, "full low-memory release did not restore all pages and references");

    /*
     * A byte range straddling the low/high allocator boundary must reserve
     * the final low page and first high page. The low page is made free
     * before reservation; the high page is still allocated. This exercises
     * outward page rounding and clipping independently in both windows.
     */
    make_full_managed_map_boot_info();
    if (multiboot2_info_validate(boot_info) != 0 ||
        memory_init((uint32_t)(uintptr_t)boot_info) != 0)
        return fail(91, "boundary-reservation map failed allocator initialization");

    const uint32_t boundary_high_pages =
        (uint32_t)((HIGH_MEMORY_LIMIT - LOW_MEMORY_LIMIT) / PAGE_SIZE);
    void *boundary_low_run = page_alloc_contiguous(full_low_pages);
    uint64_t boundary_high_page = 0u;
    if ((uintptr_t)boundary_low_run != LOW_RESERVED_PAGES * PAGE_SIZE ||
        page_alloc_high(&boundary_high_page) != 0 ||
        boundary_high_page != LOW_MEMORY_LIMIT)
        return fail(92, "could not allocate pages adjacent to the low/high boundary");

    void *boundary_low_last =
        (void *)((uintptr_t)boundary_low_run +
                 (uint64_t)(full_low_pages - 1u) * PAGE_SIZE);
    page_free(boundary_low_last);
    if (memory_free_pages() != 1u ||
        memory_high_free_pages() != boundary_high_pages - 1u)
        return fail(93, "boundary fixture did not isolate one free low page and one allocated high page");

    memory_reserve_physical_range(LOW_MEMORY_LIMIT - 1u,
                                  LOW_MEMORY_LIMIT + 1u);
    if (memory_free_pages() != 0u ||
        memory_high_free_pages() != boundary_high_pages - 1u)
        return fail(94, "cross-boundary reservation changed the wrong allocator accounting");

    page_free_high(boundary_high_page);
    if (memory_high_free_pages() != boundary_high_pages - 1u)
        return fail(95, "freeing the revoked first high page restored allocator capacity");

    for (uint32_t i = 0u; i + 1u < full_low_pages; ++i)
        page_free((void *)((uintptr_t)boundary_low_run +
                           (uint64_t)i * PAGE_SIZE));
    if (memory_free_pages() != full_low_pages - 1u)
        return fail(96, "cross-boundary low reservation was not retained after releasing the remaining run");

    void *boundary_low_next = page_alloc();
    uint64_t boundary_high_next = 0u;
    if ((uintptr_t)boundary_low_next != LOW_RESERVED_PAGES * PAGE_SIZE ||
        page_alloc_high(&boundary_high_next) != 0 ||
        boundary_high_next != LOW_MEMORY_LIMIT + PAGE_SIZE)
        return fail(97, "allocator reused a page revoked by the cross-boundary reservation");

    page_free(boundary_low_next);
    page_free_high(boundary_high_next);
    if (memory_free_pages() != full_low_pages - 1u ||
        memory_high_free_pages() != boundary_high_pages - 1u)
        return fail(98, "cross-boundary reservation regression did not restore remaining allocator accounting");

    /*
     * Initialize both supported physical-memory windows from a single
     * contiguous firmware inventory. Exercise low and high allocation
     * together, then verify both accounting domains are fully restored.
     */
    make_full_managed_map_boot_info();
    if (multiboot2_info_validate(boot_info) != 0 ||
        memory_init((uint32_t)(uintptr_t)boot_info) != 0)
        return fail(75, "combined low/high memory map failed allocator initialization");

    const uint32_t combined_low_pages =
        (uint32_t)(LOW_MEMORY_LIMIT / PAGE_SIZE) - LOW_RESERVED_PAGES;
    const uint32_t combined_high_pages =
        (uint32_t)((HIGH_MEMORY_LIMIT - LOW_MEMORY_LIMIT) / PAGE_SIZE);
    if (memory_free_pages() != combined_low_pages ||
        memory_high_free_pages() != combined_high_pages ||
        memory_high_available_bytes() !=
            HIGH_MEMORY_LIMIT - LOW_MEMORY_LIMIT)
        return fail(76, "combined memory map did not account for both complete allocator windows");

    void *combined_low = page_alloc_contiguous(2u);
    uint64_t combined_high = 0u;
    if ((uintptr_t)combined_low != LOW_RESERVED_PAGES * PAGE_SIZE ||
        page_alloc_high_contiguous(2u, &combined_high) != 0 ||
        combined_high != LOW_MEMORY_LIMIT ||
        memory_free_pages() != combined_low_pages - 2u ||
        memory_high_free_pages() != combined_high_pages - 2u)
        return fail(77, "simultaneous low/high contiguous allocation failed");

    page_free((void *)((uintptr_t)combined_low + PAGE_SIZE));
    page_free(combined_low);
    page_free_high_contiguous(combined_high, 2u);
    if (memory_free_pages() != combined_low_pages ||
        memory_high_free_pages() != combined_high_pages)
        return fail(78, "simultaneous low/high release did not restore both free-page counts");

    /*
     * The unified physical-page API prefers directly mapped low memory,
     * falls back to high memory only after the low window is exhausted,
     * and dispatches release to the matching allocator.
     */
    if (page_alloc_physical(0) != -1 ||
        memory_free_pages() != combined_low_pages ||
        memory_high_free_pages() != combined_high_pages)
        return fail(145, "unified allocator accepted a null output or changed inventory");

    page_free_physical(LOW_RESERVED_PAGES * PAGE_SIZE - PAGE_SIZE);
    if (memory_free_pages() != combined_low_pages ||
        memory_high_free_pages() != combined_high_pages)
        return fail(149, "unified release exposed a permanently reserved low page");

    uint64_t unified_page = UINT64_C(0xA5A5A5A5A5A5A5A5);
    if (page_alloc_physical(&unified_page) != 0 ||
        unified_page != LOW_RESERVED_PAGES * PAGE_SIZE ||
        memory_free_pages() != combined_low_pages - 1u)
        return fail(139, "unified page allocation did not prefer the low physical window");
    page_free_physical(unified_page + 1u);
    if (memory_free_pages() != combined_low_pages - 1u)
        return fail(146, "unaligned unified low-page release changed allocator accounting");
    page_free_physical(unified_page);
    if (memory_free_pages() != combined_low_pages)
        return fail(140, "unified low-page release did not restore low allocator accounting");
    page_free_physical(unified_page);
    if (memory_free_pages() != combined_low_pages)
        return fail(150, "repeated unified low-page release changed free-page accounting");

    void *all_low_pages = page_alloc_contiguous(combined_low_pages);
    if ((uintptr_t)all_low_pages != LOW_RESERVED_PAGES * PAGE_SIZE ||
        memory_free_pages() != 0u)
        return fail(141, "could not exhaust low memory before unified high-memory fallback");
    unified_page = UINT64_C(0xA5A5A5A5A5A5A5A5);
    if (page_alloc_physical(&unified_page) != 0 ||
        unified_page != LOW_MEMORY_LIMIT ||
        memory_free_pages() != 0u ||
        memory_high_free_pages() != combined_high_pages - 1u)
        return fail(142, "unified allocator did not fall back to the first high-memory page");
    page_free_physical(unified_page + 1u);
    if (memory_high_free_pages() != combined_high_pages - 1u)
        return fail(147, "unaligned unified high-page release changed allocator accounting");
    page_free_physical(HIGH_MEMORY_LIMIT);
    if (memory_high_free_pages() != combined_high_pages - 1u)
        return fail(148, "out-of-window unified high-page release changed allocator accounting");
    page_free_physical(unified_page);
    if (memory_high_free_pages() != combined_high_pages)
        return fail(143, "unified high-page release did not restore high allocator accounting");
    page_free_physical(unified_page);
    if (memory_high_free_pages() != combined_high_pages)
        return fail(151, "repeated unified high-page release changed free-page accounting");

    /*
     * Revoke every remaining high page after releasing the last allocation.
     * With low memory already exhausted, the unified allocator must fail
     * without overwriting its output or changing either inventory.
     */
    memory_reserve_physical_range(LOW_MEMORY_LIMIT, HIGH_MEMORY_LIMIT);
    if (memory_high_free_pages() != 0u)
        return fail(152, "full high-window reservation did not exhaust high memory");
    unified_page = UINT64_C(0x5A5A5A5A5A5A5A5A);
    if (page_alloc_physical(&unified_page) != -1 ||
        unified_page != UINT64_C(0x5A5A5A5A5A5A5A5A) ||
        memory_free_pages() != 0u ||
        memory_high_free_pages() != 0u)
        return fail(153, "unified allocation failure changed output or allocator accounting");

    for (uint32_t i = 0u; i < combined_low_pages; ++i)
        page_free((void *)((uintptr_t)all_low_pages + (uint64_t)i * PAGE_SIZE));
    if (memory_free_pages() != combined_low_pages ||
        memory_high_free_pages() != 0u)
        return fail(144, "unified allocator exhaustion regression did not restore low inventory");

    /*
     * Reserved descriptors must win even when firmware lists available RAM
     * first. The one-MiB reserved hole must split the otherwise contiguous
     * four-MiB range into two independently allocatable runs.
     */
    make_available_before_reserved_boot_info();
    if (multiboot2_info_validate(boot_info) != 0 ||
        memory_init((uint32_t)(uintptr_t)boot_info) != 0)
        return fail(59, "available-before-reserved map failed initialization");
    if (memory_free_pages() != 768u ||
        memory_high_free_pages() != 768u ||
        memory_high_available_bytes() != 4ULL * MIB ||
        page_refcount((void *)(uintptr_t)(17ULL * MIB)) != 0u ||
        page_alloc_contiguous(513u) != 0 ||
        page_alloc_high_contiguous(513u, &high_address) == 0)
        return fail(60, "available-before-reserved overlap exposed reserved pages");
    void *first_run = page_alloc_contiguous(256u);
    void *second_run = page_alloc_contiguous(256u);
    if ((uintptr_t)first_run != 16ULL * MIB ||
        (uintptr_t)second_run != 18ULL * MIB ||
        memory_free_pages() != 256u)
        return fail(61, "reserved overlap did not split the low available range");
    uint64_t first_high_run = 0u;
    uint64_t second_high_run = 0u;
    if (page_alloc_high_contiguous(256u, &first_high_run) != 0 ||
        page_alloc_high_contiguous(256u, &second_high_run) != 0 ||
        first_high_run != 512ULL * MIB ||
        second_high_run != 514ULL * MIB ||
        memory_high_free_pages() != 256u)
        return fail(63, "reserved overlap did not split the high available range");
    for (uint32_t i = 0u; i < 256u; ++i) {
        page_free((void *)((uintptr_t)first_run + (uint64_t)i * PAGE_SIZE));
        page_free((void *)((uintptr_t)second_run + (uint64_t)i * PAGE_SIZE));
    }
    page_free_high_contiguous(first_high_run, 256u);
    page_free_high_contiguous(second_high_run, 256u);
    if (memory_free_pages() != 768u || memory_high_free_pages() != 768u)
        return fail(62, "available-before-reserved allocations did not release cleanly");

    /*
     * A valid Multiboot2 map containing only reserved memory is not a usable
     * allocator configuration and must fail closed just like malformed data.
     */
    make_no_usable_memory_boot_info();
    if (multiboot2_info_validate(boot_info) != 0)
        return fail(57, "reserved-only Multiboot2 map was rejected structurally");
    if (memory_init((uint32_t)(uintptr_t)boot_info) == 0 ||
        page_alloc() != 0 || memory_free_pages() != 0u ||
        memory_high_free_pages() != 0u || memory_total_pages() != 0u ||
        memory_high_available_bytes() != 0u)
        return fail(58, "reserved-only Multiboot2 map exposed unusable memory");

    /* Invalid boot metadata must fail closed and expose no physical pages. */
    make_boot_info();
    put32(0u, 8u);
    if (expect_rejected_boot_info(53,
            "invalid Multiboot2 metadata reported successful initialization") != 0)
        return 53;

    /*
     * Exercise malformed records after a previously valid RAM map has been
     * assembled in the input buffer. Validation must reject them before any
     * low or high allocator page becomes visible.
     */
    make_boot_info();
    put32(16u, 25u);
    if (expect_rejected_boot_info(63,
            "misaligned Multiboot2 mmap entry stride was accepted") != 0)
        return 63;

    make_boot_info();
    put64(24u, UINT64_MAX - 0x7ffu);
    put64(32u, 0x1000u);
    if (expect_rejected_boot_info(65,
            "overflowing Multiboot2 physical range was accepted") != 0)
        return 65;

    make_boot_info();
    put32(180u, (uint32_t)(MODULE_START - 1u));
    if (expect_rejected_boot_info(67,
            "reversed Multiboot2 module range was accepted") != 0)
        return 67;

    make_boot_info();
    boot_info[184u] = (uint8_t)'x';
    if (expect_rejected_boot_info(69,
            "unterminated Multiboot2 module command line was accepted") != 0)
        return 69;

    /*
     * Every module tag must be validated, not only the first module in the
     * stream. Reject malformed ranges and unterminated command lines in a
     * later module before exposing any low- or high-memory pages.
     */
    make_multiple_modules_boot_info();
    put32(204u, 76u * (uint32_t)MIB - 1u);
    if (expect_rejected_boot_info(101,
            "reversed second Multiboot2 module range was accepted") != 0)
        return 101;

    make_multiple_modules_boot_info();
    boot_info[208u] = (uint8_t)'x';
    if (expect_rejected_boot_info(103,
            "unterminated second Multiboot2 module command line was accepted") != 0)
        return 103;

    if (memory_init(0u) == 0 || page_alloc() != 0 ||
        memory_free_pages() != 0u || memory_high_free_pages() != 0u)
        return fail(56, "null Multiboot2 pointer did not fail closed");

    /*
     * Exercise end-to-end ingestion of an extended Multiboot2 mmap entry.
     * The allocator must honor the advertised 32-byte stride while reading
     * only the standardized first 24 bytes of each entry.
     */
    make_extended_stride_boot_info();
    if (multiboot2_info_validate(boot_info) != 0 ||
        memory_init((uint32_t)(uintptr_t)boot_info) != 0)
        return fail(84, "extended Multiboot2 mmap entry failed validation or ingestion");
    if (memory_total_pages() != (uint32_t)(20ULL * MIB / PAGE_SIZE) ||
        memory_free_pages() != (uint32_t)(3ULL * MIB / PAGE_SIZE) ||
        memory_high_free_pages() != 0u)
        return fail(85, "extended mmap stride or reserved overlap produced incorrect accounting");
    /*
     * The larger run is 512 pages, so 257 pages may legitimately fit there.
     * Request 513 pages: total free memory is 768 pages, but neither side of
     * the reserved 1 MiB hole contains a sufficiently large contiguous run.
     */
    if (page_alloc_contiguous(513u) != 0 ||
        memory_free_pages() != (uint32_t)(3ULL * MIB / PAGE_SIZE))
        return fail(86, "extended mmap stride crossed its reserved hole or changed accounting");
    low_allocation = page_alloc_contiguous(256u);
    if ((uintptr_t)low_allocation != 16ULL * MIB ||
        memory_free_pages() != (uint32_t)(2ULL * MIB / PAGE_SIZE))
        return fail(87, "extended mmap stride exposed an incorrect first contiguous run");
    void *extended_second_run = page_alloc_contiguous(512u);
    if ((uintptr_t)extended_second_run != 18ULL * MIB ||
        memory_free_pages() != 0u)
        return fail(88, "extended mmap stride did not advance by the advertised record size");
    for (uint32_t i = 0u; i < 256u; ++i)
        page_free((void *)((uintptr_t)low_allocation + (uint64_t)i * PAGE_SIZE));
    for (uint32_t i = 0u; i < 512u; ++i)
        page_free((void *)((uintptr_t)extended_second_run + (uint64_t)i * PAGE_SIZE));
    if (memory_free_pages() != (uint32_t)(3ULL * MIB / PAGE_SIZE))
        return fail(89, "extended mmap multi-record allocations did not release cleanly");

    /*
     * High-page pins close the allocator/mapping lifetime race without
     * allocating per-page metadata for the entire 64 GiB address window.
     */
    make_full_high_map_boot_info();
    if (memory_init((uint32_t)(uintptr_t)boot_info) != 0)
        return fail(120, "full high-memory map failed before pin lifecycle tests");

    const uint32_t pin_test_full_high_pages =
        (uint32_t)((HIGH_MEMORY_LIMIT - LOW_MEMORY_LIMIT) / PAGE_SIZE);
    if (memory_high_free_pages() != pin_test_full_high_pages ||
        page_alloc_high(&high_address) != 0 ||
        high_address != LOW_MEMORY_LIMIT ||
        memory_high_page_pin(high_address) != 0 ||
        memory_high_page_pin(high_address) != 0)
        return fail(121, "allocated high page could not acquire multiple pins");

    page_free_high(high_address);
    if (memory_high_page_is_allocated(high_address) != 1 ||
        memory_high_free_pages() != pin_test_full_high_pages - 1u ||
        memory_high_page_pin(high_address) != -1 ||
        memory_high_page_unpin(high_address) != 0 ||
        memory_high_page_is_allocated(high_address) != 1 ||
        memory_high_free_pages() != pin_test_full_high_pages - 1u ||
        memory_high_page_unpin(high_address) != 0 ||
        memory_high_page_is_allocated(high_address) != 0 ||
        memory_high_free_pages() != pin_test_full_high_pages)
        return fail(122, "deferred high-page release did not wait for the final unpin");

    if (memory_high_page_pin(high_address) == 0 ||
        memory_high_page_unpin(high_address) != -1 ||
        memory_high_page_pin(LOW_MEMORY_LIMIT + 1u) != -1 ||
        memory_high_page_pin(HIGH_MEMORY_LIMIT) != -1)
        return fail(123, "high-page pin API accepted invalid or unowned page state");

    /*
     * Saturate the per-page pin reference counter. The next pin must fail
     * without wrapping the 16-bit count, and a deferred free must remain
     * unavailable until every accepted reference has been released.
     */
    if (page_alloc_high(&high_address) != 0)
        return fail(128, "high page allocation failed before pin saturation");
    for (uint32_t pin = 0u; pin < UINT16_MAX; ++pin) {
        if (memory_high_page_pin(high_address) != 0)
            return fail(129, "high-page pin reference count saturated prematurely");
    }
    if (memory_high_page_pin(high_address) != -1)
        return fail(130, "high-page pin reference count wrapped past UINT16_MAX");
    page_free_high(high_address);
    if (memory_high_page_is_allocated(high_address) != 1 ||
        memory_high_free_pages() != pin_test_full_high_pages - 1u ||
        memory_high_page_pin(high_address) != -1)
        return fail(131, "saturated high-page pins did not defer release safely");
    for (uint32_t pin = 0u; pin < UINT16_MAX; ++pin) {
        if (memory_high_page_unpin(high_address) != 0)
            return fail(132, "high-page unpin failed before all saturated references were released");
        if (pin + 1u < UINT16_MAX &&
            memory_high_page_is_allocated(high_address) != 1)
            return fail(133, "high page became reusable before the final saturated unpin");
    }
    if (memory_high_page_is_allocated(high_address) != 0 ||
        memory_high_free_pages() != pin_test_full_high_pages ||
        memory_high_page_unpin(high_address) != -1)
        return fail(134, "final saturated unpin did not release exactly one page");

    if (page_alloc_high(&high_address) != 0 ||
        memory_high_page_pin(high_address) != 0)
        return fail(124, "high page could not be prepared for reservation while pinned");
    memory_reserve_physical_range(high_address, high_address + PAGE_SIZE);
    if (memory_high_page_is_allocated(high_address) != 0 ||
        memory_range_contains_available_ram(high_address,
                                            high_address + PAGE_SIZE) != 0 ||
        memory_high_free_pages() != pin_test_full_high_pages - 1u ||
        memory_high_page_pin(high_address) != -1 ||
        memory_high_page_unpin(high_address) != 0 ||
        memory_high_page_is_allocated(high_address) != 0 ||
        memory_high_free_pages() != pin_test_full_high_pages - 1u)
        return fail(125, "reservation of a pinned high page allowed premature reuse or incorrect accounting");

    /*
     * A release request followed by a runtime reservation must remain
     * deferred across multiple independent mapping references. The final
     * unpin retires the record, but the reserved page must never re-enter
     * the allocator or its free-page inventory.
     */
    const uint32_t overlap_test_free_pages = memory_high_free_pages();
    if (overlap_test_free_pages == 0u ||
        page_alloc_high(&high_address) != 0 ||
        memory_high_page_pin(high_address) != 0 ||
        memory_high_page_pin(high_address) != 0)
        return fail(135, "could not establish multiple pins before release/reservation overlap");
    page_free_high(high_address);
    memory_reserve_physical_range(high_address, high_address + PAGE_SIZE);
    if (memory_high_page_is_allocated(high_address) != 0 ||
        memory_high_free_pages() != overlap_test_free_pages - 1u ||
        memory_high_page_pin(high_address) != -1 ||
        memory_high_page_unpin(high_address) != 0 ||
        memory_high_free_pages() != overlap_test_free_pages - 1u ||
        memory_high_page_unpin(high_address) != 0 ||
        memory_high_page_unpin(high_address) != -1 ||
        memory_high_free_pages() != overlap_test_free_pages - 1u)
        return fail(136, "overlapping release and reservation did not preserve multi-pin lifetime/accounting");
    uint64_t after_reserved_page = 0u;
    if (page_alloc_high(&after_reserved_page) != 0 ||
        after_reserved_page == high_address)
        return fail(137, "allocator reused a high page reserved while multiply pinned");
    page_free_high(after_reserved_page);
    if (memory_high_free_pages() != overlap_test_free_pages - 1u)
        return fail(138, "allocation after overlapping reservation did not restore exact inventory");

    if (high_pin_concurrent_release_test(memory_high_free_pages()) != 0)
        return fail(126, "concurrent high-page release exposed a pinned page or leaked inventory");

    if (high_pin_capacity_test(memory_high_free_pages()) != 0)
        return fail(127, "high-page pin metadata exhaustion was not fail-closed or leaked allocator inventory");

    puts("physical allocator low/high range, pin lifecycle, concurrency, and fail-closed tests passed");
    return 0;
}
