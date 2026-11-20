#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include "phys_range.h"

#define PAGE 4096ULL
#define LOW_LIMIT (256ULL * 1024ULL * 1024ULL)
#define HIGH_BASE LOW_LIMIT
#define HIGH_LIMIT (64ULL * 1024ULL * 1024ULL * 1024ULL)

static int expect(int condition, int code)
{
    if (!condition) {
        printf("physical range test failed: %d\n", code);
        return code;
    }
    return 0;
}

int main(void)
{
    uint64_t first = 0, last = 0;

    if (expect(phys_range_page_bounds(0x1001, 0x4fff, 0, LOW_LIMIT,
                                      0, &first, &last) == 1 &&
               first == 0x2000 && last == 0x4000, 1)) return 1;

    if (expect(phys_range_page_bounds(0x1001, 0x4001, 0, LOW_LIMIT,
                                      1, &first, &last) == 1 &&
               first == 0x1000 && last == 0x5000, 2)) return 2;

    if (expect(phys_range_page_bounds(LOW_LIMIT - PAGE, HIGH_BASE + PAGE,
                                      HIGH_BASE, HIGH_LIMIT, 0,
                                      &first, &last) == 1 &&
               first == HIGH_BASE && last == HIGH_BASE + PAGE, 3)) return 3;

    if (expect(phys_range_page_bounds(HIGH_LIMIT - PAGE + 1,
                                      UINT64_MAX, HIGH_BASE, HIGH_LIMIT,
                                      1, &first, &last) == 1 &&
               first == HIGH_LIMIT - PAGE && last == HIGH_LIMIT, 4)) return 4;

    if (expect(phys_range_page_bounds(UINT64_MAX - 7, UINT64_MAX,
                                      HIGH_BASE, HIGH_LIMIT, 1,
                                      &first, &last) == 0, 5)) return 5;

    if (expect(phys_range_page_bounds(0x1001, 0x1fff, 0, LOW_LIMIT,
                                      0, &first, &last) == 0, 6)) return 6;

    if (expect(phys_range_page_bounds(0x1000, 0x2000, 0, LOW_LIMIT,
                                      2, &first, &last) == -1, 7)) return 7;

    if (expect(phys_range_page_bounds(0x1000, 0x2000, 1, LOW_LIMIT,
                                      0, &first, &last) == -1, 8)) return 8;

    if (expect(phys_range_page_bounds(0x1000, 0x2000, 0, LOW_LIMIT,
                                      0, 0, &last) == -1, 9)) return 9;

    puts("physical range page-boundary tests passed");
    return 0;
}
