#include <stdint.h>
#include <stdio.h>
#include "keyboard_tty.h"
#include "tty.h"

static int fail(const char *message, int code) {
    fprintf(stderr, "KEYBOARD TTY TEST: FAIL: %s\n", message);
    return code;
}

int main(void) {
    uint8_t output[8];
    tty_init();
    if (tty_claim_session(42u, 42u) != 0)
        return fail("TTY claim failed", 1);

    if (keyboard_tty_forward_byte('h') != TTY_INPUT_NONE ||
        keyboard_tty_forward_byte('i') != TTY_INPUT_NONE ||
        keyboard_tty_forward_byte(0x08u) != TTY_INPUT_NONE ||
        keyboard_tty_forward_byte('!') != TTY_INPUT_NONE ||
        keyboard_tty_forward_byte('\n') != TTY_INPUT_LINE_READY)
        return fail("translated character forwarding failed", 2);

    int32_t count = tty_read(42u, output, sizeof(output));
    if (count != 3 || output[0] != 'h' || output[1] != '!' || output[2] != '\n')
        return fail("canonical input did not preserve forwarded bytes", 3);

    if (keyboard_tty_forward_byte('x') != TTY_INPUT_NONE ||
        keyboard_tty_forward_byte(0x03u) != TTY_INPUT_INTERRUPT ||
        keyboard_tty_forward_byte('\n') != TTY_INPUT_LINE_READY)
        return fail("Ctrl-C event forwarding failed", 4);
    count = tty_read(42u, output, sizeof(output));
    if (count != 1 || output[0] != '\n')
        return fail("interrupt event did not flush the edit line", 5);

    if (tty_release_session(42u) != 0 ||
        keyboard_tty_forward_byte('z') != TTY_ERROR)
        return fail("unclaimed TTY accepted input", 6);

    puts("KEYBOARD TTY TEST: PASS");
    return 0;
}
