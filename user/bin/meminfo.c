#include <stdint.h>
#include "user_libc.h"

int main(void) {
    uint32_t free_pages = lion_meminfo();
    puts("LionOS memory information");
    printf("Free physical pages: %u\n", free_pages);
    return 0;
}
