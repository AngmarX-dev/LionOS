#ifndef LIONOS_VFS_UAPI_H
#define LIONOS_VFS_UAPI_H

#include <stdint.h>

#define LIONOS_O_READ   1u
#define LIONOS_O_WRITE  2u
#define LIONOS_O_APPEND 4u

#define LIONOS_STAT_READ_ONLY 1u
#define LIONOS_STAT_DIRECTORY 2u
#define LIONOS_STAT_SYMLINK  4u

#define LIONOS_SYS_READLINK 45u
#define LIONOS_SYS_LSTAT    46u

#define LIONOS_SYS_OPEN   14u
#define LIONOS_SYS_CLOSE  15u
#define LIONOS_SYS_FREAD  16u
#define LIONOS_SYS_FWRITE 17u
#define LIONOS_SYS_REMOVE 18u
#define LIONOS_SYS_STAT   19u
#define LIONOS_SYS_READDIR 44u

#define LIONOS_DIRENT_UNKNOWN   0u
#define LIONOS_DIRENT_REGULAR   1u
#define LIONOS_DIRENT_DIRECTORY 2u

struct lion_dirent {
    uint32_t inode;
    uint8_t type;
    char name[256];
};

struct lion_stat {
    uint32_t size;
    uint32_t backend;
    uint32_t flags;
};

#endif
