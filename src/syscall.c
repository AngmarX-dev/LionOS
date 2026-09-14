#include <stdint.h>

static volatile uint16_t *const VGA = (uint16_t *)0xB8000;
static uint16_t cursor;

static uint32_t syscall_dispatch(uint32_t number, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    (void)arg1;
    (void)arg2;

    switch (number) {
        case 0:
            return 1; /* LionOS syscall ABI version. */

        case 1:
            /* SYS_PUTC: the kernel validates the value and owns the VGA write. */
            if (arg0 > 0xFFu) return (uint32_t)-1;
            VGA[cursor++ % (80u * 25u)] = (uint16_t)0x0F00u | (uint16_t)arg0;
            return 1;

        case 2:
            /* SYS_GETPID: process layer currently has one bootstrap process. */
            return 1;

        case 3:
            /* SYS_YIELD: scheduler is not present yet; return success. */
            return 0;

        default:
            return (uint32_t)-1;
    }
}

void syscall_init(void) {
    cursor = 0;
    (void)syscall_dispatch;
}

uint32_t syscall_handle(uint32_t number, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    return syscall_dispatch(number, arg0, arg1, arg2);
}
