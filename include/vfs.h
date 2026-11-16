#ifndef LIONOS_VFS_H
#define LIONOS_VFS_H

#include <stdint.h>

#define VFS_PATH_MAX 64u
#define VFS_IO_MAX 4096u
#define VFS_FD_MAX 32u
#define VFS_FD_INVALID (-1)

#define VFS_BACKEND_NONE 0u
#define VFS_BACKEND_RAMFS 1u
#define VFS_BACKEND_DISKFS 2u
#define VFS_BACKEND_EXT2_RO 3u

#define VFS_STAT_READ_ONLY 1u
#define VFS_STAT_DIRECTORY 2u
#define VFS_STAT_SYMLINK 4u
#define VFS_DIRENT_NAME_MAX 255u

struct vfs_dirent {
    uint32_t inode;
    uint8_t type;
    char name[VFS_DIRENT_NAME_MAX + 1u];
};

struct process;

struct vfs_stat {
    uint32_t size;
    uint32_t backend;
    uint32_t flags;
};

int vfs_init(void);
int vfs_open(const char *path, uint32_t flags);
int vfs_open_for_process(struct process *process, const char *path, uint32_t flags);
int vfs_close_for_process(struct process *process, int fd);
int vfs_read_for_process(struct process *process, int fd, void *buffer, uint32_t length);
int vfs_write_for_process(struct process *process, int fd, const void *buffer, uint32_t length);
int vfs_close(int fd);
int vfs_read(int fd, void *buffer, uint32_t length);
int vfs_write(int fd, const void *buffer, uint32_t length);
int vfs_remove(const char *path);
int vfs_stat_path(const char *path, struct vfs_stat *st);
int vfs_lstat_path(const char *path, struct vfs_stat *st);
int vfs_readlink(const char *path, char *buffer, uint32_t capacity);
int vfs_readdir(const char *path, uint32_t *offset, struct vfs_dirent *entry);
const char *vfs_name(uint32_t index);
uint32_t vfs_count(void);

#endif
