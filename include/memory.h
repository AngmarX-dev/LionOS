#ifndef LIONOS_MEMORY_H
#define LIONOS_MEMORY_H

#include <stdint.h>

void memory_init(uint32_t multiboot_info);
void *page_alloc(void);
void *page_alloc_contiguous(uint32_t count);
/* Allocates physical pages above 256 MiB; returned pointer is a physical address, not dereferenceable. */
int page_alloc_high(uint64_t *physical_address);
void page_free_high(uint64_t physical_address);
/* Exclude firmware/device-owned physical ranges from all PMM allocators. */
void memory_reserve_physical_range(uint64_t start, uint64_t end);
uint32_t memory_high_free_pages(void);
void page_free(void *page);
void page_retain(void *page);
uint32_t page_refcount(void *page);
uint32_t memory_total_pages(void);
uint32_t memory_free_pages(void);
/* Upper physical-address boundary tracked by the current PMM bitmap. */
uint64_t memory_managed_limit_bytes(void);
/* Raw Multiboot2 available-memory inventory above 256 MiB; allocator free pages are reported separately. */
uint64_t memory_high_available_bytes(void);

#endif
