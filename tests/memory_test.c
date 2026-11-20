#include <stdint.h>
#include <stdio.h>
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
#define HIGH_MEMORY_LIMIT (64ULL * 1024ULL * MIB)
#define HIGH_RESERVED_START (514ULL * MIB)
#define HIGH_RESERVED_END (515ULL * MIB)
#define MODULE_START (74ULL * MIB)
#define MODULE_END (75ULL * MIB)

static uint8_t boot_info[192] __attribute__((aligned(8)));

void spinlock_init(struct spinlock *lock)
{
    if (lock)
        lock->value = 0u;
}

void spinlock_acquire(struct spinlock *lock)
{
    if (lock)
        lock->value = 1u;
}

void spinlock_release(struct spinlock *lock)
{
    if (lock)
        lock->value = 0u;
}

uint32_t spinlock_try_acquire(struct spinlock *lock)
{
    if (!lock || lock->value)
        return 0u;
    lock->value = 1u;
    return 1u;
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
    *(uint32_t *)(void *)(boot_info + offset) = value;
}

static void put64(uint32_t offset, uint64_t value)
{
    *(uint64_t *)(void *)(boot_info + offset) = value;
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
    put32(172u, 16u);
    put32(176u, (uint32_t)MODULE_START);
    put32(180u, (uint32_t)MODULE_END);

    put32(184u, MULTIBOOT2_TAG_END);
    put32(188u, 8u);
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

static int fail(int code, const char *message)
{
    printf("physical allocator test failed (%d): %s\n", code, message);
    return code;
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

    memory_init((uint32_t)(uintptr_t)boot_info);

    if (memory_total_pages() !=
        (uint32_t)(LOW_AVAILABLE_END / PAGE_SIZE))
        return fail(3, "low-memory managed-page ceiling is incorrect");

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

    memory_reserve_physical_range(516ULL * MIB, 517ULL * MIB);
    if (memory_high_free_pages() != 1536u ||
        memory_range_contains_available_ram(516ULL * MIB,
                                            517ULL * MIB) != 0 ||
        memory_high_available_bytes() != 8ULL * MIB)
        return fail(16, "runtime high-memory reservation changed the wrong accounting");

    if (page_alloc_high(&high_address) != 0 ||
        high_address != HIGH_AVAILABLE_START)
        return fail(17, "high-page allocator did not preserve the scan after reservation");
    page_free_high(high_address);

    /*
     * Exercise both sides of the low/high allocator seam and the final page
     * below the high allocator's exclusive 64 GiB limit.
     */
    make_boundary_boot_info();
    memory_init((uint32_t)(uintptr_t)boot_info);

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
    memory_init((uint32_t)(uintptr_t)boot_info);

    if (memory_high_free_pages() !=
            (uint32_t)((HIGH_MEMORY_LIMIT - LOW_MEMORY_LIMIT) / PAGE_SIZE) ||
        memory_high_available_bytes() != HIGH_MEMORY_LIMIT - LOW_MEMORY_LIMIT)
        return fail(27, "complete high-memory window was not accounted");

    if (page_alloc_high(&high_address) != 0 ||
        high_address != LOW_MEMORY_LIMIT)
        return fail(28, "full-window allocation did not begin at 256 MiB");

    memory_reserve_physical_range(LOW_MEMORY_LIMIT + PAGE_SIZE,
                                  HIGH_MEMORY_LIMIT - PAGE_SIZE);
    if (memory_high_free_pages() != 1u ||
        memory_range_contains_available_ram(LOW_MEMORY_LIMIT + PAGE_SIZE,
                                            LOW_MEMORY_LIMIT + 2u * PAGE_SIZE) != 0 ||
        memory_range_contains_available_ram(HIGH_MEMORY_LIMIT - PAGE_SIZE,
                                            HIGH_MEMORY_LIMIT) != 1)
        return fail(29, "full-window reservation did not preserve only the final page");

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
    memory_init((uint32_t)(uintptr_t)boot_info);
    const uint32_t full_high_pages =
        (uint32_t)((HIGH_MEMORY_LIMIT - LOW_MEMORY_LIMIT) / PAGE_SIZE);

    if (page_alloc_high_contiguous(0u, &high_address) == 0 ||
        page_alloc_high_contiguous(3u, 0) == 0)
        return fail(32, "high contiguous allocator accepted invalid arguments");

    if (page_alloc_high_contiguous(3u, &high_address) != 0 ||
        high_address != LOW_MEMORY_LIMIT ||
        memory_high_free_pages() != full_high_pages - 3u)
        return fail(33, "high contiguous allocation or accounting failed");

    page_free_high_contiguous(high_address, 3u);
    if (memory_high_free_pages() != full_high_pages)
        return fail(34, "high contiguous release did not restore all pages");

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
        high_address != LOW_MEMORY_LIMIT + 3u * PAGE_SIZE ||
        memory_high_free_pages() != full_high_pages - 4u)
        return fail(38, "high contiguous allocator crossed a reserved-page hole");

    page_free_high_contiguous(high_address, 3u);
    if (memory_high_free_pages() != full_high_pages - 1u)
        return fail(39, "fragmented contiguous allocation did not release cleanly");

    page_free_high_contiguous(HIGH_MEMORY_LIMIT - PAGE_SIZE, 2u);
    page_free_high_contiguous(LOW_MEMORY_LIMIT + 1u, 1u);
    if (memory_high_free_pages() != full_high_pages - 1u)
        return fail(40, "invalid high contiguous release changed allocator state");

    /* Invalid boot metadata must fail closed and expose no physical pages. */
    put32(0u, 8u);
    memory_init((uint32_t)(uintptr_t)boot_info);
    if (memory_free_pages() != 0u || memory_high_free_pages() != 0u ||
        memory_total_pages() != 0u || memory_high_available_bytes() != 0u)
        return fail(41, "invalid Multiboot2 metadata exposed allocator memory");

    puts("physical allocator low/high range and lifecycle tests passed");
    return 0;
}
