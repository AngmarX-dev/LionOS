#ifndef LIONOS_PHYS_RANGE_H
#define LIONOS_PHYS_RANGE_H

#include <stdint.h>

/*
 * Convert a physical byte range into page-aligned bounds inside an aligned
 * managed interval. Available ranges include only complete pages; reserved
 * ranges conservatively include every page touched by the byte range.
 *
 * The two output pointers must be non-null and distinct. Their values are
 * updated only when the function returns 1; return values 0 and -1 leave both
 * outputs unchanged.
 *
 * Returns 1 when the result is non-empty, 0 when there is no managed page,
 * and -1 for invalid arguments or interval definitions.
 */
int phys_range_page_bounds(uint64_t start, uint64_t end,
                           uint64_t limit_start, uint64_t limit_end,
                           int reserve, uint64_t *page_start,
                           uint64_t *page_end);

#endif
