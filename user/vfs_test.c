#include <stdint.h>
#include <limits.h>
#include "user_libc.h"
#include "posix_compat.h"

static int libc_memory_test(void) {
    char overlap[8] = "abcdef";
    (void)memmove(overlap + 2, overlap, 4u);
    if (memcmp(overlap, "ababcd", 6u) != 0) return 1;
    (void)memmove(overlap, overlap + 2, 4u);
    if (memcmp(overlap, "abcdcd", 6u) != 0) return 2;

    if (malloc(0u) != 0 || malloc(UINT32_MAX) != 0) return 3;
    if (atoi("  +42") != 42 || atoi("-42") != -42 ||
        atoi("2147483648") != INT_MAX || atoi("-2147483649") != INT_MIN)
        return 9;
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

static int directory_stream_test(void) {
    DIR *root;
    DIR *nested;
    struct lion_dirent *entry;
    struct lion_dirent raw_entry;
    uint32_t offset = 1u;
    int saw_readme = 0;
    int saw_subdir = 0;
    int saw_nested = 0;

    if (opendir("/ext2/README.TXT") != 0)
        return 20;
    if (closedir(0) >= 0)
        return 21;
    root = opendir("/ext2");
    if (!root)
        return 22;
    while ((entry = readdir(root)) != 0) {
        if (strcmp(entry->name, "README.TXT") == 0 &&
            entry->type == LIONOS_DIRENT_REGULAR)
            saw_readme = 1;
        if (strcmp(entry->name, "subdir") == 0 &&
            entry->type == LIONOS_DIRENT_DIRECTORY)
            saw_subdir = 1;
    }
    if (!saw_readme || !saw_subdir || readdir(root) != 0)
        return 23;
    if (closedir(root) != 0)
        return 24;

    nested = opendir("/ext2/subdir");
    if (!nested)
        return 25;
    while ((entry = readdir(nested)) != 0) {
        if (strcmp(entry->name, "nested.txt") == 0 &&
            entry->type == LIONOS_DIRENT_REGULAR)
            saw_nested = 1;
    }
    if (!saw_nested || closedir(nested) != 0)
        return 26;

    if (lion_sys_readdir("/ext2", 0, &raw_entry) >= 0)
        return 27;
    if (lion_sys_readdir("/ext2", &offset, &raw_entry) >= 0 ||
        offset != 1u)
        return 28;
    offset = 0u;
    if (lion_sys_readdir("/ext2", &offset, 0) >= 0 ||
        offset != 0u)
        return 29;
    return 0;
}


static int ext2_symlink_syscall_test(void) {
    static const char link_path[] = "/ext2/readme-link";
    static const char expected_target[] = "README.TXT";
    static const char loop_path[] = "/ext2/loop-a";
    static const char intermediate_path[] = "/ext2/subdir-link/nested.txt";
    static const char direct_path[] = "/ext2/subdir/nested.txt";
    struct lion_stat link_status;
    struct lion_stat target_status;
    struct lion_stat loop_status;
    struct lion_stat intermediate_status;
    char target[sizeof(expected_target) - 1u];
    char short_target[5] = {'?', '?', '?', '?', '!'};

    if (lion_lstat(link_path, &link_status) < 0 ||
        (link_status.flags & LIONOS_STAT_SYMLINK) == 0u ||
        link_status.size != sizeof(expected_target) - 1u)
        return 30;
    if (lion_stat(link_path, &target_status) < 0 ||
        (target_status.flags & LIONOS_STAT_SYMLINK) != 0u)
        return 31;
    if (lion_readlink(link_path, target, sizeof(target)) !=
            (int32_t)(sizeof(expected_target) - 1u) ||
        memcmp(target, expected_target, sizeof(target)) != 0)
        return 32;
    if (lion_readlink(link_path, short_target, 4u) != 4 ||
        memcmp(short_target, expected_target, 4u) != 0 ||
        short_target[4] != '!')
        return 33;
    if (lion_readlink("/ext2/README.TXT", target, sizeof(target)) >= 0)
        return 34;
    if (lion_readlink(link_path, target, 0u) >= 0 ||
        lion_readlink(link_path, 0, 4u) >= 0 ||
        lion_lstat(0, &link_status) >= 0 ||
        lion_lstat(link_path, 0) >= 0)
        return 35;
    if (lion_lstat(loop_path, &loop_status) < 0 ||
        (loop_status.flags & LIONOS_STAT_SYMLINK) == 0u ||
        loop_status.size != 6u || lion_stat(loop_path, &target_status) >= 0)
        return 36;
    if (lion_lstat(intermediate_path, &intermediate_status) < 0 ||
        (intermediate_status.flags & LIONOS_STAT_SYMLINK) != 0u ||
        lion_stat(direct_path, &target_status) < 0 ||
        intermediate_status.size != target_status.size)
        return 37;
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
    int directory_result = directory_stream_test();
    if (directory_result != 0)
        return directory_result;
    return ext2_symlink_syscall_test();
}
