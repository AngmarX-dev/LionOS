#include <stdint.h>
#include "posix_compat.h"
#include "user_api.h"

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
