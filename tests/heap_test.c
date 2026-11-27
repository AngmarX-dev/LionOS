#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include "heap.h"
#include "memory.h"

#define PAGE_SIZE 4096u
#define WORKERS 8u
#define ITERATIONS 12000u
#define MAX_ACTIVE (WORKERS * 2u)

static pthread_mutex_t pmm_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t active_mutex = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t next_page = 0x10000000u;
static uint64_t allocated_pages;
static uint64_t freed_pages;
static void *active[MAX_ACTIVE];
static size_t active_count;
static int test_failed;

void *page_alloc_contiguous(uint32_t count)
{
    void *result = 0;
    if (!count)
        return 0;
    pthread_mutex_lock(&pmm_mutex);
    if ((uint64_t)next_page + (uint64_t)count * PAGE_SIZE <= UINT32_MAX) {
        result = (void *)next_page;
        next_page += (uintptr_t)count * PAGE_SIZE;
        allocated_pages += count;
    }
    pthread_mutex_unlock(&pmm_mutex);
    return result;
}

void page_free(void *page)
{
    if (!page)
        return;
    pthread_mutex_lock(&pmm_mutex);
    ++freed_pages;
    pthread_mutex_unlock(&pmm_mutex);
}

static int active_add(void *ptr)
{
    int ok = 1;
    pthread_mutex_lock(&active_mutex);
    if (active_count >= MAX_ACTIVE)
        ok = 0;
    for (size_t i = 0; ok && i < active_count; ++i)
        if (active[i] == ptr)
            ok = 0;
    if (ok)
        active[active_count++] = ptr;
    pthread_mutex_unlock(&active_mutex);
    return ok;
}

static void active_remove(void *ptr)
{
    pthread_mutex_lock(&active_mutex);
    size_t i;
    for (i = 0; i < active_count; ++i)
        if (active[i] == ptr)
            break;
    if (i == active_count)
        __atomic_store_n(&test_failed, 1, __ATOMIC_RELAXED);
    else
        active[i] = active[--active_count];
    pthread_mutex_unlock(&active_mutex);
}

static void *worker(void *arg)
{
    uintptr_t id = (uintptr_t)arg;
    for (uint32_t i = 0; i < ITERATIONS; ++i) {
        size_t size = (size_t)(((i * 37u + (uint32_t)id * 101u) % 12288u) + 1u);
        void *ptr = kmalloc(size);
        if (!ptr)
            continue;
        if (!active_add(ptr)) {
            __atomic_store_n(&test_failed, 1, __ATOMIC_RELAXED);
            continue;
        }
        active_remove(ptr);
        kfree(ptr);
    }
    return 0;
}

static int fail(int code, const char *message)
{
    fprintf(stderr, "heap_test: %s\n", message);
    return code;
}

int main(void)
{
    void *slots[1024];
    if (kmalloc(1u) != 0)
        return fail(1, "allocation succeeded before heap_init");
    heap_init();
    if (kmalloc(0u) != 0)
        return fail(2, "zero-sized allocation was accepted");
    if (kmalloc((size_t)UINT32_MAX) != 0)
        return fail(3, "overflowing allocation size was accepted");

    void *one = kmalloc(1u);
    void *exact = kmalloc(PAGE_SIZE);
    void *two = kmalloc(PAGE_SIZE + 1u);
    if (!one || !exact || !two || one == exact || one == two ||
        exact == two || allocated_pages != 4u)
        return fail(4, "page rounding or unique allocation failed");

    kfree(0);
    kfree((void *)(uintptr_t)0x12345000u);
    kfree(one);
    kfree(one);
    if (freed_pages != 1u)
        return fail(5, "null, unknown, or duplicate free changed PMM accounting");
    kfree(exact);
    kfree(two);
    if (freed_pages != allocated_pages)
        return fail(6, "multi-page allocations did not release exact page counts");

    for (uint32_t i = 0; i < 1024u; ++i) {
        slots[i] = kmalloc(1u);
        if (!slots[i])
            return fail(7, "allocation table filled before its declared capacity");
    }
    if (kmalloc(1u) != 0)
        return fail(8, "allocation beyond table capacity was accepted");
    for (uint32_t i = 0; i < 1024u; ++i)
        kfree(slots[i]);
    if (freed_pages != allocated_pages)
        return fail(9, "capacity test leaked or double-freed PMM pages");

    one = kmalloc(PAGE_SIZE + 1u);
    if (!one)
        return fail(10, "allocation failed before idempotent initialization test");
    heap_init();
    kfree(one);
    if (freed_pages != allocated_pages)
        return fail(11, "repeated heap_init lost a live allocation record");

    pthread_t threads[WORKERS];
    for (uintptr_t i = 0; i < WORKERS; ++i)
        if (pthread_create(&threads[i], 0, worker, (void *)i) != 0)
            return fail(12, "could not create concurrency worker");
    for (uint32_t i = 0; i < WORKERS; ++i)
        if (pthread_join(threads[i], 0) != 0)
            return fail(13, "could not join concurrency worker");

    if (__atomic_load_n(&test_failed, __ATOMIC_RELAXED) ||
        active_count != 0u || freed_pages != allocated_pages)
        return fail(14, "concurrent allocation/free lost, duplicated, or leaked an allocation");

    puts("heap allocation, bounds, capacity, idempotent init, and concurrent free tests passed");
    return 0;
}
