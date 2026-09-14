#include <stdint.h>
#include "io.h"

void syscall_init(void) {
    /* INT 0x80 is installed by idt_init(). This function is reserved for the syscall ABI. */
}

uint32_t syscall_dispatch(uint32_t number, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    switch (number) {
        case 0: return 0; /* get kernel ABI version */
        case 1: return 1; /* future console write */
        default: return (uint32_t)-1;
    }
}
