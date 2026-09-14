#include "user_libc.h"

int main(void) {
    char name[64];
    uint32_t index = 0;
    while (lion_getfile(index, name, sizeof(name)) >= 0) {
        struct lion_stat st;
        if (lion_stat(name, &st) == 0)
            printf("%s %u bytes\n", name, st.size);
        else
            puts(name);
        ++index;
    }
    return 0;
}
