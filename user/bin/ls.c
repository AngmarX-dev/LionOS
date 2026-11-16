#include "user_libc.h"
#include "posix_compat.h"

int main(void) {
    char name[64];
    uint32_t index = 0;
    DIR *directory;
    struct lion_dirent *entry;

    puts("RAMFS / LionFS:");
    while (lion_getfile(index, name, sizeof(name)) >= 0) {
        struct lion_stat st;
        if (lion_stat(name, &st) == 0)
            printf("%s %u bytes\n", name, st.size);
        else
            puts(name);
        ++index;
    }

    directory = opendir("/ext2");
    if (!directory)
        return 0;
    puts("ext2 (read-only):");
    while ((entry = readdir(directory)) != 0) {
        printf("%s%s\n", entry->name,
               entry->type == LIONOS_DIRENT_DIRECTORY ? "/" : "");
    }
    (void)closedir(directory);
    return 0;
}
