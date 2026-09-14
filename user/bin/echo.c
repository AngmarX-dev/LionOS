#include "user_libc.h"

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (i > 1) putchar(' ');
        puts(argv[i]);
        if (i + 1 < argc) { /* puts added a newline; keep utility simple below. */ }
    }
    return 0;
}
