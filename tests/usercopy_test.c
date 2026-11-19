#include <stdint.h>
#include <stdio.h>
#include "paging.h"
#include "usercopy.h"

static uint32_t cow_resolver_calls;
static uint32_t simulate_resolver_race;

int process_resolve_cow(uint32_t address) {
    ++cow_resolver_calls;
    if (simulate_resolver_race) {
        int resolved;
        simulate_resolver_race = 0u;
        resolved = paging_resolve_cow(paging_current_address_space(), address);
        return resolved == 0 ? -1 : resolved;
    }
    return paging_resolve_cow(paging_current_address_space(), address);
}

#define PAGE_SIZE 4096u
#define TEST_PAGES 4u
#define PTE_WRITABLE 0x2u
#define PTE_COW 0x200u

struct fake_page {
    uint32_t address;
    uint32_t flags;
    int present;
};

static struct fake_page pages[TEST_PAGES];

uint32_t paging_current_address_space(void) { return 1u; }

int paging_get_user_page(uint32_t pd, uint32_t address,
                         uint32_t *physical, uint32_t *flags) {
    if (pd != 1u) return -1;
    for (uint32_t i = 0u; i < TEST_PAGES; ++i) {
        if (pages[i].present && pages[i].address == address) {
            if (physical) *physical = 0x200000u + i * PAGE_SIZE;
            if (flags) *flags = pages[i].flags;
            return 0;
        }
    }
    return -1;
}

int paging_resolve_cow(uint32_t pd, uint32_t address) {
    if (pd != 1u) return -1;
    for (uint32_t i = 0u; i < TEST_PAGES; ++i) {
        if (pages[i].present && pages[i].address == address &&
            (pages[i].flags & PTE_COW)) {
            pages[i].flags = (pages[i].flags | PTE_WRITABLE) & ~PTE_COW;
            return 0;
        }
    }
    return -1;
}

static void reset_pages(void) {
    cow_resolver_calls = 0u;
    simulate_resolver_race = 0u;
    for (uint32_t i = 0u; i < TEST_PAGES; ++i) {
        pages[i].address = 0x10000000u + i * PAGE_SIZE;
        pages[i].flags = PTE_WRITABLE;
        pages[i].present = 1;
    }
}

static int check(const char *name, int actual, int expected) {
    if (actual == expected) return 0;
    fprintf(stderr, "usercopy test failed: %s (actual=%d expected=%d)\n",
            name, actual, expected);
    return 1;
}

int main(void) {
    int failures = 0;
    reset_pages();

    failures += check("valid one-byte range",
                      usercopy_range_valid(0x10000000u, 1u), 1);
    failures += check("zero-length in-range pointer",
                      usercopy_range_valid(0x10000000u, 0u), 1);
    failures += check("zero-length upper-bound pointer",
                      usercopy_range_valid(0xC0000000u, 0u), 0);
    failures += check("below user range",
                      usercopy_range_valid(0x0FFFFFFFu, 1u), 0);
    failures += check("crosses user limit",
                      usercopy_range_valid(0xBFFFFFFFu, 2u), 0);
    failures += check("large overflow length",
                      usercopy_range_valid(0x10000000u, 0xFFFFFFFFu), 0);

    failures += check("mapped readable page",
                      usercopy_validate_read(0x10000010u, 16u), 1);
    failures += check("unmapped page",
                      usercopy_validate_read(0x10004000u, 1u), 0);
    failures += check("cross-page missing mapping",
                      usercopy_validate_read(0x10003FF0u, 32u), 0);
    failures += check("writable page",
                      usercopy_validate_write(0x10000020u, 8u), 1);

    reset_pages();
    pages[0].flags = PTE_COW;
    failures += check("COW page becomes writable",
                      usercopy_validate_write(0x10000020u, 8u), 1);
    failures += check("COW uses process metadata resolver",
                      (int)cow_resolver_calls, 1);
    failures += check("COW flag cleared",
                      (pages[0].flags & PTE_COW) != 0u, 0);

    reset_pages();
    pages[0].flags = PTE_COW;
    pages[1].flags = PTE_COW;
    failures += check("cross-page COW range",
                      usercopy_validate_write(0x10000FF0u, 32u), 1);
    failures += check("cross-page COW resolves both owners",
                      (int)cow_resolver_calls, 2);
    failures += check("first COW page writable",
                      (pages[0].flags & PTE_WRITABLE) != 0u, 1);
    failures += check("second COW page writable",
                      (pages[1].flags & PTE_WRITABLE) != 0u, 1);

    reset_pages();
    pages[0].flags = PTE_COW;
    pages[1].present = 0;
    failures += check("missing second page rejects full write range",
                      usercopy_validate_write(0x10000FF0u, 32u), 0);
    failures += check("preflight leaves first COW page unchanged",
                      (pages[0].flags & PTE_COW) != 0u, 1);
    failures += check("preflight does not call resolver",
                      (int)cow_resolver_calls, 0);

    reset_pages();
    pages[0].flags = PTE_COW;
    simulate_resolver_race = 1u;
    failures += check("concurrent COW resolution is rechecked",
                      usercopy_validate_write(0x10000020u, 8u), 1);
    failures += check("concurrent COW page is writable",
                      (pages[0].flags & PTE_WRITABLE) != 0u, 1);

    reset_pages();
    pages[0].flags = 0u;
    failures += check("read-only page remains readable",
                      usercopy_validate_read(0x10000020u, 8u), 1);
    failures += check("read-only non-COW page rejected",
                      usercopy_validate_write(0x10000020u, 8u), 0);

    if (failures != 0) return 1;
    puts("Usercopy validation tests: PASS");
    return 0;
}
