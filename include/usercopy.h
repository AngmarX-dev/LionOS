#ifndef LIONOS_USERCOPY_H
#define LIONOS_USERCOPY_H

#include <stdint.h>

int usercopy_range_valid(uint32_t address, uint32_t length);
int usercopy_validate_read(uint32_t address, uint32_t length);
int usercopy_validate_write(uint32_t address, uint32_t length);
int usercopy_copy_from(void *kernel_dst, uint32_t user_src, uint32_t length);
int usercopy_copy_to(uint32_t user_dst, const void *kernel_src, uint32_t length);
int usercopy_copy_string(char *kernel_dst, uint32_t dst_size, uint32_t user_src);

#endif
