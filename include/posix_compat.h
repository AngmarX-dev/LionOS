#ifndef LIONOS_POSIX_COMPAT_H
#define LIONOS_POSIX_COMPAT_H

#include <stdint.h>
#include "vfs_uapi.h"

/*
 * Small POSIX-inspired file API over native LionOS descriptors.
 * These flags and calls are LionOS wrappers, not a POSIX or Linux ABI.
 */
#define LION_POSIX_O_READ   LIONOS_O_READ
#define LION_POSIX_O_WRITE  LIONOS_O_WRITE
#define LION_POSIX_O_APPEND LIONOS_O_APPEND
#define LION_POSIX_IO_MAX   4096u

int32_t lion_posix_open(const char *path, uint32_t flags);
int32_t lion_posix_read(int32_t fd, void *buffer, uint32_t count);
int32_t lion_posix_write(int32_t fd, const void *buffer, uint32_t count);
int32_t lion_posix_close(int32_t fd);
int32_t lion_posix_stat(const char *path, struct lion_stat *status);
int32_t lion_posix_unlink(const char *path);

/* Read-only directory stream API; DIR objects are owned by this library. */
typedef struct lion_DIR DIR;
DIR *opendir(const char *path);
struct lion_dirent *readdir(DIR *directory);
int32_t closedir(DIR *directory);

#endif
