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
    if (atoi("-42") != -42 || strcmp("lion", "lion") != 0 || memcmp("abc", "abc", 3) != 0) {
        puts("USERLAND TEST: FAIL (libc)");
        return 1;
    }
    printf("USERLAND TEST: PASS pid=%u files=%u\n", lion_getpid(), (uint32_t)0);
    return 0;
}
