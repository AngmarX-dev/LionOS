#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "tty.h"

extern uint32_t tty_test_sleep_count;
extern uint32_t tty_test_wakeup_count;
extern uint32_t tty_test_signal_count;
extern uint32_t tty_test_signal_sid;
extern uint32_t tty_test_signal_pgid;
extern uint32_t tty_test_signal_number;
extern char tty_test_output[2048];
extern uint32_t tty_test_output_length;

static int fail(const char *message, int code) {
    fprintf(stderr, "TTY TEST: FAIL: %s\n", message);
    return code;
}

int main(void) {
    uint8_t output[TTY_INPUT_QUEUE_CAPACITY];

    tty_init();
    if (tty_session_owner() != 0u)
        return fail("initial owner must be empty", 1);
    tty_test_sleep_count = 0u;
    tty_test_wakeup_count = 0u;
    tty_test_signal_count = 0u;
    tty_test_output_length = 0u;
    if (tty_claim_session(0u, 0u) == 0 ||
        tty_claim_session(10u, 11u) == 0)
        return fail("invalid session claim accepted", 2);
    if (tty_claim_session(10u, 10u) != 0)
        return fail("valid session claim failed", 3);
    if (tty_claim_session(20u, 20u) == 0)
        return fail("second session stole console TTY", 4);
    if (tty_read_blocking(10u, output, sizeof(output)) != TTY_READ_BLOCKED ||
        tty_test_sleep_count != 1u)
        return fail("empty stdin did not sleep", 22);
    if (tty_input_byte('r') != TTY_INPUT_NONE ||
        tty_test_wakeup_count != 0u ||
        tty_input_byte('\n') != TTY_INPUT_LINE_READY ||
        tty_test_wakeup_count != 1u)
        return fail("completed line did not wake stdin", 23);
    if (tty_read_blocking(10u, output, sizeof(output)) != 2 ||
        output[0] != 'r' || output[1] != '\n')
        return fail("blocking stdin did not consume completed line", 24);
    if (tty_test_output_length != 2u ||
        tty_test_output[0] != 'r' || tty_test_output[1] != '\n')
        return fail("canonical input echo mismatch", 26);
    if (tty_get_foreground_pgid(10u) != 10)
        return fail("initial foreground PGID mismatch", 5);
    uint32_t attributes = 0u;
    uint32_t vmin = 0u;
    if (tty_get_attributes(10u, &attributes, &vmin) != 0 || attributes != (TTY_ATTR_ECHO | TTY_ATTR_ISIG | TTY_ATTR_ICANON) || vmin != 1u)
        return fail("default TTY attributes mismatch", 30);
    if (tty_set_attributes(10u, TTY_ATTR_ECHO | 0x80u, 1u) == 0 || tty_set_attributes(20u, 0u, 1u) == 0)
        return fail("invalid TTY attributes accepted", 31);
    if (tty_set_attributes(10u, TTY_ATTR_ISIG | TTY_ATTR_ICANON, 1u) != 0 || tty_input_byte('n') != TTY_INPUT_NONE || tty_input_byte('\n') != TTY_INPUT_LINE_READY)
        return fail("echo-disable mode setup failed", 32);
    tty_flush_echo(10u);
    if (tty_test_output_length != 2u || tty_read(10u, output, sizeof(output)) != 2)
        return fail("disabled echo changed canonical input", 33);
    if (tty_set_attributes(10u, TTY_ATTR_ECHO | TTY_ATTR_ICANON, 1u) != 0 || tty_input_byte(0x03u) != TTY_INPUT_NONE || tty_input_byte('\n') != TTY_INPUT_LINE_READY)
        return fail("ISIG-disable mode treated Ctrl-C as signal", 34);
    tty_flush_echo(10u);
    if (tty_test_signal_count != 0u || tty_read(10u, output, sizeof(output)) != 2 || output[0] != 0x03u || output[1] != '\n')
        return fail("ISIG-disable mode did not preserve Ctrl-C byte", 35);
    if (tty_set_attributes(10u, TTY_ATTR_ECHO | TTY_ATTR_ISIG | TTY_ATTR_ICANON, 1u) != 0)
        return fail("TTY attributes restore failed", 36);
    if (tty_set_foreground_pgid(10u, 0u) == 0 ||
        tty_set_foreground_pgid(20u, 21u) == 0)
        return fail("invalid foreground PGID update accepted", 6);
    if (tty_set_foreground_pgid(10u, 12u) != 0 ||
        tty_get_foreground_pgid(10u) != 12)
        return fail("foreground PGID update failed", 7);

    if (tty_read(10u, output, sizeof(output)) != TTY_READ_EMPTY)
        return fail("empty read did not return empty sentinel", 8);
    tty_test_output_length = 0u;
    if (tty_input_byte('a') != TTY_INPUT_NONE ||
        tty_input_byte('b') != TTY_INPUT_NONE ||
        tty_input_byte(0x7Fu) != TTY_INPUT_NONE ||
        tty_input_byte('c') != TTY_INPUT_NONE ||
        tty_input_byte('\r') != TTY_INPUT_LINE_READY)
        return fail("canonical editing/newline failed", 9);
    tty_flush_echo(10u);
    if (tty_test_output_length != 7u ||
        memcmp(tty_test_output, "ab\b \bc\n", 7u) != 0)
        return fail("canonical editing echo/backspace mismatch", 27);
    if (tty_read(20u, output, sizeof(output)) != TTY_ERROR)
        return fail("non-owner read accepted", 10);
    if (tty_read(10u, output, 2u) != 2 ||
        output[0] != 'a' || output[1] != 'c')
        return fail("partial canonical read mismatch", 11);
    if (tty_read(10u, output, sizeof(output)) != 1 || output[0] != '\n')
        return fail("remaining canonical input mismatch", 12);

    tty_test_output_length = 0u;
    if (tty_input_byte('x') != TTY_INPUT_NONE ||
        tty_input_byte(0x03u) != TTY_INPUT_INTERRUPT ||
        tty_input_byte('\n') != TTY_INPUT_LINE_READY)
        return fail("interrupt character handling failed", 13);
    tty_flush_echo(10u);
    if (tty_test_output_length != 5u ||
        memcmp(tty_test_output, "x^C\n\n", 5u) != 0)
        return fail("Ctrl-C visual echo mismatch", 28);
    if (tty_test_signal_count != 1u || tty_test_signal_sid != 10u ||
        tty_test_signal_pgid != 12u || tty_test_signal_number != 2u)
        return fail("Ctrl-C was not routed to the foreground process group", 25);
    if (tty_read(10u, output, sizeof(output)) != 1 || output[0] != '\n')
        return fail("interrupt did not flush pending edit", 14);

    tty_test_output_length = 0u;
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
    if (tty_test_output_length != TTY_EDIT_LINE_CAPACITY ||
        tty_test_output[TTY_EDIT_LINE_CAPACITY - 1u] != '\n')
        return fail("maximum canonical echo length mismatch", 29);

    if (tty_input_byte('p') != TTY_INPUT_NONE ||
        tty_set_attributes(10u, TTY_ATTR_ECHO | TTY_ATTR_ISIG, 1u) == 0 ||
        tty_input_byte(0x7Fu) != TTY_INPUT_NONE)
        return fail("mode switch discarded pending canonical edit", 37);
    tty_flush_echo(10u);
    tty_test_output_length = 0u;
    if (tty_set_attributes(10u, TTY_ATTR_ECHO | TTY_ATTR_ISIG, 1u) != 0)
        return fail("noncanonical mode setup failed", 43);
    if (tty_set_attributes(10u, TTY_ATTR_ECHO | TTY_ATTR_ISIG, TTY_VMIN_MAX + 1u) == 0 ||
        tty_set_attributes(10u, TTY_ATTR_ECHO | TTY_ATTR_ISIG, 3u) != 0 ||
        tty_read_blocking(10u, output, sizeof(output)) != TTY_READ_BLOCKED ||
        tty_test_sleep_count != 2u)
        return fail("VMIN range validation or blocking failed", 46);
    if (tty_set_attributes(10u, TTY_ATTR_ECHO | TTY_ATTR_ISIG, 0u) != 0 ||
        tty_test_wakeup_count != 7u ||
        tty_read_blocking(10u, output, sizeof(output)) != 0)
        return fail("VMIN zero did not release blocked reader", 47);
    if (tty_set_attributes(10u, TTY_ATTR_ECHO | TTY_ATTR_ISIG, 3u) != 0 ||
        tty_read_blocking(10u, output, sizeof(output)) != TTY_READ_BLOCKED ||
        tty_test_sleep_count != 3u)
        return fail("VMIN threshold reconfiguration failed", 48);
    if (tty_input_byte('r') != TTY_INPUT_DATA_READY ||
        tty_test_wakeup_count != 9u ||
        tty_read_blocking(10u, output, sizeof(output)) != TTY_READ_BLOCKED ||
        tty_test_sleep_count != 4u)
        return fail("VMIN threshold did not retain partial input", 44);
    if (tty_input_byte('s') != TTY_INPUT_DATA_READY ||
        tty_test_wakeup_count != 10u ||
        tty_read_blocking(10u, output, sizeof(output)) != TTY_READ_BLOCKED ||
        tty_test_sleep_count != 5u)
        return fail("VMIN threshold woke before minimum", 45);
    if (tty_input_byte('t') != TTY_INPUT_DATA_READY ||
        tty_test_wakeup_count != 11u ||
        tty_read_blocking(10u, output, sizeof(output)) != 3 ||
        output[0] != 'r' || output[1] != 's' || output[2] != 't')
        return fail("noncanonical byte did not wake/read immediately", 39);
    if (tty_input_byte(0x7Fu) != TTY_INPUT_DATA_READY ||
        tty_input_byte('\r') != TTY_INPUT_DATA_READY ||
        tty_read(10u, output, sizeof(output)) != 2 ||
        output[0] != 0x7Fu || output[1] != '\r')
        return fail("noncanonical mode applied canonical editing", 40);
    tty_flush_echo(10u);
    if (tty_test_output_length != 3u || tty_test_output[0] != 'r' ||
        tty_test_output[1] != 0x7Fu || tty_test_output[2] != '\r')
        return fail("noncanonical echo mismatch", 41);
    if (tty_set_attributes(10u, TTY_ATTR_ECHO | TTY_ATTR_ISIG | TTY_ATTR_ICANON, 1u) != 0)
        return fail("canonical mode restore failed", 42);

    if (tty_release_session(20u) == 0 ||
        tty_release_session(10u) != 0 ||
        tty_test_wakeup_count != 14u ||
        tty_session_owner() != 0u ||
        tty_get_foreground_pgid(10u) != TTY_ERROR)
        return fail("session release lifecycle failed", 20);
    if (tty_input_byte('q') != TTY_ERROR)
        return fail("input accepted without owner", 21);

    puts("TTY TEST: PASS");
    return 0;
}
