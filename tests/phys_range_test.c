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


static uint64_t next_random(uint64_t *state)
{
    uint64_t value = *state;
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    *state = value;
    return value;
}

static int randomized_reference_test(void)
{
    const uint64_t limit_start = 0x4000ULL;
    const uint64_t limit_end = 0x24000ULL;
    uint64_t state = 0x9e3779b97f4a7c15ULL;

    for (uint32_t iteration = 0u; iteration < 50000u; ++iteration) {
        uint64_t start = next_random(&state) % 0x28000ULL;
        uint64_t end = next_random(&state) % 0x28000ULL;
        for (int reserve = 0; reserve <= 1; ++reserve) {
            uint64_t first = UINT64_MAX;
            uint64_t last = UINT64_MAX;
            int actual = phys_range_page_bounds(start, end, limit_start,
                                                limit_end, reserve,
                                                &first, &last);
            uint64_t clipped_start = start < limit_start
                                         ? limit_start : start;
            uint64_t clipped_end = end > limit_end ? limit_end : end;
            int expected = 0;
            uint64_t expected_first = 0u;
            uint64_t expected_last = 0u;

            if (end > start && clipped_end > clipped_start) {
                if (reserve) {
                    expected_first = clipped_start & ~(PAGE - 1u);
                    expected_last = (clipped_end + PAGE - 1u) & ~(PAGE - 1u);
                    if (expected_last > limit_end)
                        expected_last = limit_end;
                } else {
                    expected_first = (clipped_start + PAGE - 1u) & ~(PAGE - 1u);
                    expected_last = clipped_end & ~(PAGE - 1u);
                }
                if (expected_last > expected_first)
                    expected = 1;
            }

            if (actual != expected)
                return 1;
            if (expected &&
                (first != expected_first || last != expected_last))
                return 2;
        }
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

    first = 0x1122334455667788ULL;
    if (expect(phys_range_page_bounds(0x1000, 0x2000, 0, LOW_LIMIT,
                                      0, &first, &first) == -1 &&
               first == 0x1122334455667788ULL, 10)) return 10;

    if (randomized_reference_test() != 0) return 11;

    puts("physical range page-boundary tests passed");
    return 0;
}
