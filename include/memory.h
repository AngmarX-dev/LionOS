#ifndef LIONOS_MEMORY_H
#define LIONOS_MEMORY_H

#include <stdint.h>

void memory_init(uint32_t multiboot_info);
void *page_alloc(void);
void *page_alloc_contiguous(uint32_t count);
void page_free(void *page);
uint32_t memory_total_pages(void);
uint32_t memory_free_pages(void);

#endif
