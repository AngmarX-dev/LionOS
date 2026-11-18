#include <stdio.h>
#include "memory_type.h"

int main(void)
{
    if (memory_type_resolver_selftest() != 0) {
        fputs("memory type resolver self-test failed\n", stderr);
        return 1;
    }
    puts("memory type resolver self-test passed");
    return 0;
}
