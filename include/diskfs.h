#ifndef LIONOS_DISKFS_H
#define LIONOS_DISKFS_H

#include <stdint.h>

#define DISKFS_MAX_FILES 32u
#define DISKFS_NAME_MAX 64u
#define DISKFS_MAX_FILE_SIZE 4096u

int diskfs_init(void);
int diskfs_available(void);
uint32_t diskfs_count(void);
const char *diskfs_name(uint32_t index);
int diskfs_exists(const char *name);
uint32_t diskfs_size(const char *name);
int diskfs_read(const char *name, void *buffer, uint32_t capacity);
int diskfs_write(const char *name, const void *data, uint32_t size);
int diskfs_remove(const char *name);

#endif
