#include <stdint.h>
#include "posix_compat.h"
#include "user_api.h"
#include "user_libc.h"

#define LION_POSIX_VALID_FLAGS (LION_POSIX_O_READ | LION_POSIX_O_WRITE | LION_POSIX_O_APPEND)

int32_t lion_posix_open(const char *path, uint32_t flags) {
    if (!path || (flags & ~LION_POSIX_VALID_FLAGS) != 0u)
        return -1;
    if ((flags & (LION_POSIX_O_READ | LION_POSIX_O_WRITE)) == 0u)
        return -1;
    if ((flags & LION_POSIX_O_APPEND) != 0u &&
        (flags & LION_POSIX_O_WRITE) == 0u)
        return -1;
    return lion_open(path, flags);
}

int32_t lion_posix_read(int32_t fd, void *buffer, uint32_t count) {
    if (fd < 0 || count > LION_POSIX_IO_MAX || (count != 0u && !buffer))
        return -1;
    if (count == 0u)
        return 0;
    return lion_fread(fd, buffer, count);
}

int32_t lion_posix_write(int32_t fd, const void *buffer, uint32_t count) {
    if (fd < 0 || count > LION_POSIX_IO_MAX || (count != 0u && !buffer))
        return -1;
    if (count == 0u)
        return 0;
    return lion_fwrite(fd, buffer, count);
}

int32_t lion_posix_close(int32_t fd) {
    if (fd < 0)
        return -1;
    return lion_close(fd);
}

int32_t lion_posix_stat(const char *path, struct lion_stat *status) {
    if (!path || !status)
        return -1;
    return lion_stat(path, status);
}

int32_t lion_posix_unlink(const char *path) {
    if (!path)
        return -1;
    return lion_remove(path);
}

struct lion_DIR {
    char path[LION_POSIX_PATH_MAX];
    uint32_t offset;
    uint32_t active;
    struct lion_dirent entry;
};

DIR *opendir(const char *path) {
    struct lion_stat status;
    struct lion_DIR *directory;
    uint32_t length = 0u;

    if (!path)
        return 0;
    while (length < LION_POSIX_PATH_MAX && path[length])
        ++length;
    if (length == 0u || length >= LION_POSIX_PATH_MAX)
        return 0;
    if (lion_posix_stat(path, &status) < 0 ||
        (status.flags & LIONOS_STAT_DIRECTORY) == 0u)
        return 0;

    directory = (struct lion_DIR *)malloc((uint32_t)sizeof(*directory));
    if (!directory)
        return 0;
    memset(directory, 0, (uint32_t)sizeof(*directory));
    for (uint32_t i = 0u; i <= length; ++i)
        directory->path[i] = path[i];
    directory->active = 1u;
    return directory;
}

struct lion_dirent *readdir(DIR *stream) {
    struct lion_DIR *directory = (struct lion_DIR *)stream;
    int32_t result;

    if (!directory || directory->active != 1u)
        return 0;
    result = lion_sys_readdir(directory->path, &directory->offset,
                              &directory->entry);
    if (result != 1)
        return 0;
    return &directory->entry;
}

int32_t closedir(DIR *stream) {
    struct lion_DIR *directory = (struct lion_DIR *)stream;

    if (!directory || directory->active != 1u)
        return -1;
    directory->active = 0u;
    free(directory);
    return 0;
}
