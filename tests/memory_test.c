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
static uint8_t boot_info[200]
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

    put32(0u, sizeof(boot_info));
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
     * A runtime reservation must revoke an allocated low page, and a
     * zero-reference reserved page must not be resurrected by page_retain.
     */
    low_allocation = page_alloc();
    if ((uintptr_t)low_allocation != LOW_AVAILABLE_START)
        return fail(43, "low allocator did not reuse the first released page");
    uint32_t low_free_before_reserve = memory_free_pages();
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
                                            HIGH_AVAILABLE_START + PAGE_SIZE) != 1)
        return fail(14, "high-page availability query or allocation accounting failed");

    page_free_high(high_address);
    if (memory_high_free_pages() != 1792u)
        return fail(15, "high-page release did not restore the free count");

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
        memory_high_free_pages() != 1533u)
        return fail(100, "high allocator could not prepare an allocated reservation run");

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
        memory_range_contains_available_ram(HIGH_AVAILABLE_START + 2u * PAGE_SIZE,
                                            HIGH_AVAILABLE_START + 3u * PAGE_SIZE) != 1)
        return fail(101, "reserving an allocated high page corrupted availability or accounting");

    page_free_high_contiguous(high_address, 3u);
    if (memory_high_free_pages() != 1535u)
        return fail(102, "releasing a run with a revoked high page restored an incorrect free count");

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

    puts("physical allocator low/high range, concurrency, lifecycle, and fail-closed tests passed");
    return 0;
}
