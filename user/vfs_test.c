#include <stdint.h>
#include "user_libc.h"
#include "posix_compat.h"

static int libc_memory_test(void) {
    char overlap[8] = "abcdef";
    (void)memmove(overlap + 2, overlap, 4u);
    if (memcmp(overlap, "ababcd", 6u) != 0) return 1;
    (void)memmove(overlap, overlap + 2, 4u);
    if (memcmp(overlap, "abcdcd", 6u) != 0) return 2;

    if (malloc(0u) != 0 || malloc(UINT32_MAX) != 0) return 3;
    void *first = malloc(64u);
    void *second = malloc(64u);
    if (!first || !second || first == second ||
        (((uintptr_t)first | (uintptr_t)second) & 7u) != 0u) return 4;

    free(first);
    void *reused = malloc(32u);
    if (reused != first) return 5;
    free(reused);
    free(second);

    void *left = malloc(128u);
    void *middle = malloc(128u);
    void *right = malloc(128u);
    if (!left || !middle || !right) return 6;
    free(left);
    free(middle);
    void *merged = malloc(240u);
    if (merged != left) return 7;
    free(merged);
    free(right);

    if (printf("%u %d %x", 1u, -2, 10u) != 6) return 8;
    putchar('\n');
    return 0;
}

int main(void) {
    if (libc_memory_test() != 0) return 19;

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
