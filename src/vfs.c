#include <stdint.h>
#include "diskfs.h"
#include "ramfs.h"
#include "vfs.h"

#define VFS_F_READ  1u
#define VFS_F_WRITE 2u
#define VFS_F_APPEND 4u

struct vfs_fd {
    uint32_t used;
    uint32_t backend;
    uint32_t flags;
    uint32_t offset;
    char path[VFS_PATH_MAX];
};

static struct vfs_fd fds[VFS_FD_MAX];

static int eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static int copy_path(char *dst, const char *src) {
    if (!dst || !src || !*src) return -1;
    uint32_t i = 0;
    while (src[i] && i < VFS_PATH_MAX - 1u) {
        if (src[i] == '/') return -1;
        dst[i] = src[i];
        ++i;
    }
    if (src[i] != 0) return -1;
    dst[i] = 0;
    return 0;
}

static uint32_t backend_for(const char *path) {
    if (ramfs_data(path)) return VFS_BACKEND_RAMFS;
    if (diskfs_available() && diskfs_size(path) != 0u) return VFS_BACKEND_DISKFS;
    return VFS_BACKEND_NONE;
}

static int find_fd(int fd) {
    return fd >= 0 && fd < (int)VFS_FD_MAX && fds[fd].used;
}

int vfs_init(void) {
    for (uint32_t i = 0; i < VFS_FD_MAX; ++i) fds[i].used = 0;
    return 0;
}

int vfs_open(const char *path, uint32_t flags) {
    char clean[VFS_PATH_MAX];
    if (copy_path(clean, path) < 0) return VFS_FD_INVALID;

    uint32_t backend = backend_for(clean);
    if (backend == VFS_BACKEND_NONE) {
        if (!(flags & VFS_F_WRITE)) return VFS_FD_INVALID;
        if (diskfs_available()) {
            static const char empty[] = "";
            if (diskfs_write(clean, empty, 0) == 0) backend = VFS_BACKEND_DISKFS;
        }
        if (backend == VFS_BACKEND_NONE) {
            if (ramfs_write(clean, "", 0) == 0) backend = VFS_BACKEND_RAMFS;
            else return VFS_FD_INVALID;
        }
    }

    for (uint32_t i = 0; i < VFS_FD_MAX; ++i) {
        if (fds[i].used) continue;
        fds[i].used = 1;
        fds[i].backend = backend;
        fds[i].flags = flags;
        fds[i].offset = (flags & VFS_F_APPEND) ?
            (backend == VFS_BACKEND_DISKFS ? diskfs_size(clean) : ramfs_size(clean)) : 0u;
        uint32_t j = 0; while (clean[j]) { fds[i].path[j] = clean[j]; ++j; } fds[i].path[j] = 0;
        return (int)i;
    }
    return VFS_FD_INVALID;
}

int vfs_close(int fd) {
    if (!find_fd(fd)) return -1;
    fds[fd].used = 0;
    return 0;
}

int vfs_read(int fd, void *buffer, uint32_t length) {
    if (!find_fd(fd) || !buffer || length > VFS_IO_MAX) return -1;
    struct vfs_fd *f = &fds[fd];
    uint32_t size = f->backend == VFS_BACKEND_DISKFS ? diskfs_size(f->path) : ramfs_size(f->path);
    if (f->offset >= size) return 0;
    uint32_t wanted = length;
    if (wanted > size - f->offset) wanted = size - f->offset;

    if (f->backend == VFS_BACKEND_DISKFS) {
        uint8_t temp[VFS_IO_MAX];
        if (diskfs_read(f->path, temp, sizeof(temp)) < 0) return -1;
        for (uint32_t i = 0; i < wanted; ++i) ((uint8_t *)buffer)[i] = temp[f->offset + i];
    } else {
        const uint8_t *src = (const uint8_t *)ramfs_data(f->path);
        if (!src) return -1;
        for (uint32_t i = 0; i < wanted; ++i) ((uint8_t *)buffer)[i] = src[f->offset + i];
    }
    f->offset += wanted;
    return (int)wanted;
}

int vfs_write(int fd, const void *buffer, uint32_t length) {
    if (!find_fd(fd) || !buffer || length > VFS_IO_MAX) return -1;
    struct vfs_fd *f = &fds[fd];
    if (!(f->flags & VFS_F_WRITE)) return -1;
    if (f->offset != 0u) return -1;

    int result;
    if (f->backend == VFS_BACKEND_DISKFS) result = diskfs_write(f->path, buffer, length);
    else result = ramfs_write(f->path, buffer, length);
    if (result < 0) return -1;
    f->offset = length;
    return (int)length;
}

int vfs_remove(const char *path) {
    if (ramfs_data(path)) return ramfs_remove(path);
    if (diskfs_available() && diskfs_size(path) != 0u) return diskfs_remove(path);
    return -1;
}

int vfs_stat_path(const char *path, struct vfs_stat *st) {
    if (!path || !st) return -1;
    if (ramfs_data(path)) {
        st->size = ramfs_size(path); st->backend = VFS_BACKEND_RAMFS; st->flags = 0; return 0;
    }
    if (diskfs_available() && diskfs_size(path) != 0u) {
        st->size = diskfs_size(path); st->backend = VFS_BACKEND_DISKFS; st->flags = 0; return 0;
    }
    return -1;
}

const char *vfs_name(uint32_t index) {
    uint32_t ram_count = ramfs_count();
    if (index < ram_count) return ramfs_name(index);
    return diskfs_name(index - ram_count);
}

uint32_t vfs_count(void) {
    return ramfs_count() + (diskfs_available() ? diskfs_count() : 0u);
}
