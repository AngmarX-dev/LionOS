#include "keyboard_tty.h"
#include "tty.h"

int32_t keyboard_tty_forward_byte(uint8_t byte) {
    return tty_input_byte(byte);
}
