#include <stdint.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* RET is valid code, but this buffer lives on the NX-protected user stack. */
    volatile uint8_t stack_code[1];
    stack_code[0] = 0xC3u;
    void (*entry)(void) = (void (*)(void))(uintptr_t)&stack_code[0];
    entry();

    /* Reaching this point means execution from the stack was not blocked. */
    return 1;
}
