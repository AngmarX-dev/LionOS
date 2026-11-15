#include <stdint.h>
#include "user_libc.h"

int main(void) {
    char name[64];
    struct lion_stat st;

    if (lion_getfile(0, name, sizeof(name)) < 0) {
        puts("USERLAND TEST: FAIL (getfile)");
        return 1;
    }
    if (lion_stat(name, &st) < 0) {
        puts("USERLAND TEST: FAIL (stat)");
        return 1;
    }
    if (strcmp(name, "readme.txt") != 0) {
        puts("USERLAND TEST: FAIL (first file)");
        return 1;
    }
    if (st.size == 0) {
        puts("USERLAND TEST: FAIL (size)");
        return 1;
    }
    if (atoi("-42") != -42 || strcmp("lion", "lion") != 0 ||
        memcmp("abc", "abc", 3u) != 0) {
        puts("USERLAND TEST: FAIL (libc strings)");
        return 1;
    }

    char overlap[8] = "abcdef";
    (void)memmove(overlap + 2, overlap, 4u);
    if (memcmp(overlap, "ababcd", 6u) != 0) {
        puts("USERLAND TEST: FAIL (memmove forward overlap)");
        return 1;
    }
    (void)memmove(overlap, overlap + 2, 4u);
    if (memcmp(overlap, "abcdcd", 6u) != 0) {
        puts("USERLAND TEST: FAIL (memmove backward overlap)");
        return 1;
    }

    if (malloc(0u) != 0 || malloc(UINT32_MAX) != 0) {
        puts("USERLAND TEST: FAIL (malloc bounds)");
        return 1;
    }
    void *first = malloc(64u);
    void *second = malloc(64u);
    if (!first || !second || first == second ||
        (((uintptr_t)first | (uintptr_t)second) & 7u) != 0u) {
        puts("USERLAND TEST: FAIL (malloc allocation/alignment)");
        return 1;
    }
    free(first);
    void *reused = malloc(32u);
    if (reused != first) {
        puts("USERLAND TEST: FAIL (free reuse)");
        return 1;
    }
    free(reused);
    free(second);

    void *left = malloc(128u);
    void *middle = malloc(128u);
    void *right = malloc(128u);
    if (!left || !middle || !right) {
        puts("USERLAND TEST: FAIL (allocator fragmentation setup)");
        return 1;
    }
    free(left);
    free(middle);
    void *merged = malloc(240u);
    if (merged != left) {
        puts("USERLAND TEST: FAIL (allocator coalescing)");
        return 1;
    }
    free(merged);
    free(right);

    if (printf("%u %d %x", 1u, -2, 10u) != 6) {
        puts("USERLAND TEST: FAIL (printf count)");
        return 1;
    }
    putchar('\n');
    printf("USERLAND TEST: PASS pid=%u files=%u\n", lion_getpid(), (uint32_t)0);
    return 0;
}
