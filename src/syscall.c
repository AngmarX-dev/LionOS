#include <stdint.h>

static uint32_t syscall_dispatch(uint32_t number, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    (void)arg1;
    (void)arg2;
    switch (number) {
        case 0: return 1; /* LionOS syscall ABI version */
        case 1: return arg0; /* placeholder console-write return value */
        default: return (uint32_t)-1;
    }
}

void syscall_init(void) {
    /* INT 0x80 is installed by idt_init(); dispatch occurs in interrupt_dispatch(). */
    (void)syscall_dispatch;
}

uint32_t syscall_handle(uint32_t number, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    return syscall_dispatch(number, arg0, arg1, arg2);
}
