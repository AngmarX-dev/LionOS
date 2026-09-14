#include "user_libc.h"

static int cat_file(const char *path) {
    char buffer[4096];
    int32_t fd = lion_open(path, LIONOS_O_READ);
    if (fd < 0) {
        printf("cat: %s: not found\n", path);
        return 1;
    }
    for (;;) {
        int32_t n = lion_fread(fd, buffer, sizeof(buffer));
        if (n < 0) { lion_close(fd); return 1; }
        if (n == 0) break;
        lion_write(buffer, (uint32_t)n);
        if ((uint32_t)n < sizeof(buffer)) break;
    }
    lion_close(fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { puts("usage: cat <file>..."); return 1; }
    int result = 0;
    for (int i = 1; i < argc; ++i) if (cat_file(argv[i])) result = 1;
    return result;
}
