#include <stdint.h>
#include "user_libc.h"

int main(void) {
    uint32_t free_pages = lion_meminfo();
    if (free_pages == LIONOS_SYSCALL_ERR) {
        puts("meminfo: memory query failed");
        return 1;
    }
    puts("LionOS memory information");
    printf("Free physical pages: %u\n", free_pages);
    return 0;
}
