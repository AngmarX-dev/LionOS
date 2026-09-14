#ifndef LIONOS_RAMFS_H
#define LIONOS_RAMFS_H

#include <stdint.h>

void ramfs_init(void);
uint32_t ramfs_count(void);
const char *ramfs_name(uint32_t index);
const char *ramfs_data(const char *name);
uint32_t ramfs_size(const char *name);
int ramfs_exists(const char *name);
int ramfs_write(const char *name, const char *data, uint32_t size);
int ramfs_remove(const char *name);

#endif
