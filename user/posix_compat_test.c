#include <stdint.h>
#include "user_libc.h"
#include "posix_compat.h"

int main(void) {
    static const char path[] = "userspace-posix-test.txt";
    static const char first[] = "LionOS POSIX-inspired read/write ";
    static const char second[] = "append OK\n";
    static const char expected[] = "LionOS POSIX-inspired read/write append OK\n";
    char buffer[96];
    struct lion_stat status;

    if (lion_posix_open(path, 0u) >= 0)
        return 1;
    if (lion_posix_open(path, LION_POSIX_O_READ | 0x80000000u) >= 0)
        return 2;
    if (lion_posix_read(-1, buffer, 1u) >= 0)
        return 3;
    if (lion_posix_write(-1, buffer, 1u) >= 0)
        return 4;
    if (lion_posix_read(0, 0, 0u) != 0 || lion_posix_write(0, 0, 0u) != 0)
        return 5;

    int32_t fd = lion_posix_open(path, LION_POSIX_O_WRITE);
    if (fd < 0)
        return 6;
    if (lion_posix_write(fd, first, sizeof(first) - 1u) != (int32_t)(sizeof(first) - 1u)) {
        (void)lion_posix_close(fd);
        return 7;
    }
    if (lion_posix_close(fd) < 0)
        return 8;

    fd = lion_posix_open(path, LION_POSIX_O_WRITE | LION_POSIX_O_APPEND);
    if (fd < 0)
        return 9;
    if (lion_posix_write(fd, second, sizeof(second) - 1u) != (int32_t)(sizeof(second) - 1u)) {
        (void)lion_posix_close(fd);
        return 10;
    }
    if (lion_posix_close(fd) < 0)
        return 11;

    if (lion_posix_stat(path, &status) < 0 ||
        status.size != sizeof(expected) - 1u)
        return 12;

    fd = lion_posix_open(path, LION_POSIX_O_READ);
    if (fd < 0)
        return 13;
    int32_t n = lion_posix_read(fd, buffer, sizeof(buffer) - 1u);
    if (n != (int32_t)(sizeof(expected) - 1u)) {
        (void)lion_posix_close(fd);
        return 14;
    }
    buffer[n] = 0;
    if (strcmp(buffer, expected) != 0) {
        (void)lion_posix_close(fd);
        return 15;
    }
    if (lion_posix_close(fd) < 0)
        return 16;
    if (lion_posix_unlink(path) < 0)
        return 17;
    if (lion_posix_stat(path, &status) >= 0)
        return 18;
    return 0;
}
