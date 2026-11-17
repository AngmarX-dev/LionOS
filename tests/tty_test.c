#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "tty.h"

static int fail(const char *message, int code) {
    fprintf(stderr, "TTY TEST: FAIL: %s\n", message);
    return code;
}

int main(void) {
    uint8_t output[TTY_INPUT_QUEUE_CAPACITY];

    tty_init();
    if (tty_session_owner() != 0u)
        return fail("initial owner must be empty", 1);
    if (tty_claim_session(0u, 0u) == 0 ||
        tty_claim_session(10u, 11u) == 0)
        return fail("invalid session claim accepted", 2);
    if (tty_claim_session(10u, 10u) != 0)
        return fail("valid session claim failed", 3);
    {
        static const uint8_t message[] = {'O', 'K'};
        if (tty_write(20u, message, sizeof(message)) != TTY_ERROR ||
            tty_write(10u, message, sizeof(message)) != (int32_t)sizeof(message) ||
            tty_write(10u, 0, 0u) != 0 ||
            tty_write(10u, 0, 1u) != TTY_ERROR ||
            tty_write(10u, message, 4097u) != TTY_ERROR)
            return fail("session-scoped output validation failed", 22);
    }
    if (tty_claim_session(20u, 20u) == 0)
        return fail("second session stole console TTY", 4);
    if (tty_get_foreground_pgid(10u) != 10)
        return fail("initial foreground PGID mismatch", 5);
    if (tty_set_foreground_pgid(10u, 0u) == 0 ||
        tty_set_foreground_pgid(20u, 21u) == 0)
        return fail("invalid foreground PGID update accepted", 6);
    if (tty_set_foreground_pgid(10u, 12u) != 0 ||
        tty_get_foreground_pgid(10u) != 12)
        return fail("foreground PGID update failed", 7);

    if (tty_read(10u, output, sizeof(output)) != TTY_READ_EMPTY)
        return fail("empty read did not return empty sentinel", 8);
    if (tty_input_byte('a') != TTY_INPUT_NONE ||
        tty_input_byte('b') != TTY_INPUT_NONE ||
        tty_input_byte(0x7Fu) != TTY_INPUT_NONE ||
        tty_input_byte('c') != TTY_INPUT_NONE ||
        tty_input_byte('\r') != TTY_INPUT_LINE_READY)
        return fail("canonical editing/newline failed", 9);
    if (tty_read(20u, output, sizeof(output)) != TTY_ERROR)
        return fail("non-owner read accepted", 10);
    if (tty_read(10u, output, 2u) != 2 ||
        output[0] != 'a' || output[1] != 'c')
        return fail("partial canonical read mismatch", 11);
    if (tty_read(10u, output, sizeof(output)) != 1 || output[0] != '\n')
        return fail("remaining canonical input mismatch", 12);

    if (tty_input_byte('x') != TTY_INPUT_NONE ||
        tty_input_byte(0x03u) != TTY_INPUT_INTERRUPT ||
        tty_input_byte('\n') != TTY_INPUT_LINE_READY)
        return fail("interrupt character handling failed", 13);
    if (tty_read(10u, output, sizeof(output)) != 1 || output[0] != '\n')
        return fail("interrupt did not flush pending edit", 14);

    for (uint32_t i = 0u; i < TTY_EDIT_LINE_CAPACITY - 1u; ++i) {
        if (tty_input_byte('z') != TTY_INPUT_NONE)
            return fail("maximum line fill failed", 15);
    }
    if (tty_input_byte('z') == TTY_INPUT_NONE)
        return fail("oversized line accepted", 16);
    if (tty_input_byte('\n') != TTY_INPUT_LINE_READY)
        return fail("maximum valid line could not be committed", 17);
    if (tty_read(10u, output, sizeof(output)) != (int32_t)TTY_EDIT_LINE_CAPACITY)
        return fail("maximum line read length mismatch", 18);
    if (output[TTY_EDIT_LINE_CAPACITY - 1u] != '\n')
        return fail("maximum line missing newline", 19);

    if (tty_release_session(20u) == 0 ||
        tty_release_session(10u) != 0 ||
        tty_session_owner() != 0u ||
        tty_get_foreground_pgid(10u) != TTY_ERROR)
        return fail("session release lifecycle failed", 20);
    if (tty_input_byte('q') != TTY_ERROR)
        return fail("input accepted without owner", 21);

    puts("TTY TEST: PASS");
    return 0;
}
