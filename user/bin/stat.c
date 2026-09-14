#include "user_libc.h"

int main(int argc, char **argv) {
    if (argc != 2) { puts("usage: stat <file>"); return 1; }
    struct lion_stat st;
    if (lion_stat(argv[1], &st) < 0) {
        printf("stat: %s: not found\n", argv[1]);
        return 1;
    }
    printf("name: %s\nsize: %u bytes\nbackend: %u\nflags: %x\n", argv[1], st.size, st.backend, st.flags);
    return 0;
}
