#ifndef LIONOS_PAGING_H
#define LIONOS_PAGING_H

#include <stdint.h>

/* Semantic user mapping flag; paging translates it to hardware NX when available. */
#define PAGING_USER_EXECUTABLE 0x400u

void paging_init(void);
uint32_t paging_kernel_directory(void);
uint32_t paging_create_address_space(void);
int paging_map_kernel_page(uint32_t virtual_address, uint64_t physical_address, uint32_t flags);
int paging_map_kernel_range(uint32_t virtual_address, uint64_t physical_address,
                            uint32_t size, uint32_t flags);
int paging_kernel_range_selftest(void);
void paging_tlb_shootdown_interrupt(void);
int paging_map_high_page(uint64_t physical_address, uint32_t flags, uint32_t *virtual_address);
int paging_unmap_high_page(uint32_t virtual_address);
int paging_map_user_page_in(uint32_t page_directory, uint32_t virtual_address,
                            uint32_t physical_address, uint32_t flags);
int paging_set_user_page_flags(uint32_t page_directory, uint32_t virtual_address, uint32_t flags);
int paging_mark_cow(uint32_t page_directory, uint32_t virtual_address);
int paging_resolve_cow(uint32_t page_directory, uint32_t virtual_address);
int paging_get_user_page(uint32_t page_directory, uint32_t virtual_address,
                         uint32_t *physical_address, uint32_t *flags);
int paging_user_page_executable(uint32_t page_directory, uint32_t virtual_address);
void paging_destroy_address_space(uint32_t page_directory);
void paging_switch_address_space(uint32_t page_directory);
uint32_t paging_current_address_space(void);

#endif
