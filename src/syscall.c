#include <stdint.h>
#include "process.h"
#include "syscall.h"

static volatile uint16_t *const VGA = (uint16_t *)0xB8000;
static uint16_t cursor;

static uint32_t syscall_dispatch(uint32_t number, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    (void)arg1;
    (void)arg2;

    switch (number) {
        case SYS_ABI_VERSION:
            return 1;

        case SYS_PUTC:
            if (arg0 > 0xFFu) return SYSCALL_ERR;
            VGA[cursor++ % (80u * 25u)] = (uint16_t)0x0F00u | (uint16_t)arg0;
            return SYSCALL_OK;

        case SYS_GETPID:
            return process_current_pid();

        case SYS_YIELD:
            /* The dispatcher is ready for a scheduler; no context switch yet. */
            __asm__ volatile ("pause");
            return SYSCALL_OK;

        case SYS_EXIT:
            process_exit_current();
            return SYSCALL_OK;

        default:
            return SYSCALL_ERR;
    }
}

void syscall_init(void) {
    cursor = 0;
    (void)syscall_dispatch;
}

uint32_t syscall_handle(uint32_t number, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    return syscall_dispatch(number, arg0, arg1, arg2);
}
