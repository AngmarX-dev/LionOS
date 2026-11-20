#include "phys_range.h"

#define PHYS_PAGE_SIZE 4096ULL
#define PHYS_PAGE_MASK (PHYS_PAGE_SIZE - 1ULL)

int phys_range_page_bounds(uint64_t start, uint64_t end,
                           uint64_t limit_start, uint64_t limit_end,
                           int reserve, uint64_t *page_start,
                           uint64_t *page_end)
{
    if (!page_start || !page_end || limit_end <= limit_start ||
        (limit_start & PHYS_PAGE_MASK) != 0ULL ||
        (limit_end & PHYS_PAGE_MASK) != 0ULL ||
        (reserve != 0 && reserve != 1))
        return -1;

    if (end <= start || end <= limit_start || start >= limit_end)
        return 0;

    if (start < limit_start)
        start = limit_start;
    if (end > limit_end)
        end = limit_end;

    if (reserve) {
        start &= ~PHYS_PAGE_MASK;
        if ((end & PHYS_PAGE_MASK) != 0ULL)
            end = (end & ~PHYS_PAGE_MASK) + PHYS_PAGE_SIZE;
    } else {
        if ((start & PHYS_PAGE_MASK) != 0ULL)
            start = (start & ~PHYS_PAGE_MASK) + PHYS_PAGE_SIZE;
        end &= ~PHYS_PAGE_MASK;
    }

    if (start < limit_start)
        start = limit_start;
    if (end > limit_end)
        end = limit_end;
    if (end <= start)
        return 0;

    *page_start = start;
    *page_end = end;
    return 1;
}
