#include "user_libc.h"

int main(int argc, char **argv) {
    if (argc < 2) { puts("usage: rm <file>..."); return 1; }
    int result = 0;
    for (int i = 1; i < argc; ++i) {
        if (lion_remove(argv[i]) < 0) {
            printf("rm: %s: failed\n", argv[i]);
            result = 1;
        }
    }
    return result;
}
