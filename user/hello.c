#include <stdint.h>
#include "user_libc.h"

int main(int argc, char **argv) {
    puts("Hello from LionOS userspace!");
    puts("C runtime + libc are working.");

    puts("Process information:");
    (void)lion_write("  PID: ", 7u);
    char digit = (char)('0' + (lion_getpid() % 10u));
    putchar(digit);
    putchar('\n');

    if (argc > 0 && argv && argv[0]) {
        (void)lion_write("  argv[0]: ", 11u);
        puts(argv[0]);
    }

    return 0;
}
