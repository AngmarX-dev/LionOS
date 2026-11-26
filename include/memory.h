#ifndef LIONOS_MEMORY_H
#define LIONOS_MEMORY_H

#include <stdint.h>

/* Returns 0 after complete validation; -1 leaves all allocator pages unavailable. */
int memory_init(uint32_t multiboot_info);
void *page_alloc(void);
/* Allocate/free one page through the unified 0..64 GiB physical-memory API. */
int page_alloc_physical(uint64_t *physical_address);
void page_free_physical(uint64_t physical_address);
void *page_alloc_contiguous(uint32_t count);
/* Allocates physical pages above 256 MiB; returned address is physical, not directly dereferenceable. */
int page_alloc_high(uint64_t *physical_address);
/*
 * Allocates/releases a physically contiguous high-memory run; release skips
 * pages already reserved. The output address is written only on success.
 */
int page_alloc_high_contiguous(uint32_t count, uint64_t *physical_address);
void page_free_high_contiguous(uint64_t physical_address, uint32_t count);
void page_free_high(uint64_t physical_address);
/* Exclude firmware/device-owned physical ranges from all PMM allocators. */
void memory_reserve_physical_range(uint64_t start, uint64_t end);
uint32_t memory_high_free_pages(void);
/* Returns 1 for an allocated high-RAM page, 0 for unallocated/reserved RAM,
 * and -1 for an unaligned or out-of-window physical address. */
int memory_high_page_is_allocated(uint64_t physical_address);
/* Pin an allocated high-RAM page; fails for invalid pages or exhausted pin slots. */
int memory_high_page_pin(uint64_t physical_address);
/* Release one pin; deferred frees and reservations take effect on the final unpin. */
int memory_high_page_unpin(uint64_t physical_address);
void page_free(void *page);
/* Low-page references saturate at UINT16_MAX; invalid or unallocated pages are ignored. */
void page_retain(void *page);
uint32_t page_refcount(void *page);
uint32_t memory_total_pages(void);
uint32_t memory_free_pages(void);
/* Upper physical-address boundary tracked by the current PMM bitmap. */
uint64_t memory_managed_limit_bytes(void);
/* Returns 1 if any page in the aligned range is available RAM, 0 if none,
 * and -1 when the range is invalid or outside the tracked high-memory map. */
int memory_range_contains_available_ram(uint64_t start, uint64_t end);
/* Raw Multiboot2 available-memory inventory above 256 MiB; allocator free pages are reported separately. */
uint64_t memory_high_available_bytes(void);

#endif
