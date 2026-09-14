#ifndef LIONOS_PAGING_H
#define LIONOS_PAGING_H

#include <stdint.h>

void paging_init(void);
uint32_t paging_kernel_directory(void);
uint32_t paging_create_address_space(void);
int paging_map_user_page_in(uint32_t page_directory, uint32_t virtual_address,
                            uint32_t physical_address, uint32_t flags);
int paging_get_user_page(uint32_t page_directory, uint32_t virtual_address,
                         uint32_t *physical_address, uint32_t *flags);
void paging_destroy_address_space(uint32_t page_directory);
void paging_switch_address_space(uint32_t page_directory);
uint32_t paging_current_address_space(void);

#endif
