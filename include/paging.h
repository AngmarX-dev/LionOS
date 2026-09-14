#ifndef LIONOS_PAGING_H
#define LIONOS_PAGING_H

#include <stdint.h>

void paging_init(void);
int paging_map_user_page(uint32_t virtual_address, uint32_t physical_address, uint32_t flags);

#endif
