#include <stdint.h>
#include "memory.h"
#include "paging.h"
#include "process.h"
#include "syscall.h"
#include "user.h"

#define USER_CODE_VA   0x00400000u
#define USER_STACK_VA  0x00401000u
#define USER_STACK_TOP 0x00402000u

static void enter_user_mode(uint32_t entry, uint32_t stack) __attribute__((noreturn));

static void enter_user_mode(uint32_t entry, uint32_t stack) {
    __asm__ volatile (
        "cli\n"
        "mov $0x2B, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "pushl $0x2B\n"
        "pushl %[user_stack]\n"
        "pushfl\n"
        "orl $0x200, (%%esp)\n"
        "pushl $0x23\n"
        "pushl %[user_entry]\n"
        "iret\n"
        :
        : [user_entry] "r"(entry), [user_stack] "r"(stack)
        : "ax", "memory"
    );
    __builtin_unreachable();
}

int user_mode_test(void) {
    uint8_t *code = (uint8_t *)page_alloc();
    uint8_t *stack = (uint8_t *)page_alloc();

    if (!code || !stack) {
        if (code) page_free(code);
        if (stack) page_free(stack);
        return -1;
    }

    /* SYS_PUTC('!'), SYS_YIELD, SYS_EXIT, then halt in user mode. */
    static const uint8_t program[] = {
        0xB8, SYS_PUTC, 0x00, 0x00, 0x00,
        0xBB, 0x21, 0x00, 0x00, 0x00,
        0xCD, 0x80,
        0xB8, SYS_YIELD, 0x00, 0x00, 0x00,
        0xCD, 0x80,
        0xB8, SYS_EXIT, 0x00, 0x00, 0x00,
        0xCD, 0x80,
        0xFA,
        0xF4,
        0xEB, 0xFC
    };

    for (uint32_t i = 0; i < sizeof(program); ++i) code[i] = program[i];
    for (uint32_t i = 0; i < 4096u; ++i) stack[i] = 0;

    if (paging_map_user_page(USER_CODE_VA, (uint32_t)code, 0x5u) != 0 ||
        paging_map_user_page(USER_STACK_VA, (uint32_t)stack, 0x7u) != 0) {
        page_free(code);
        page_free(stack);
        return -1;
    }

    struct process *process = process_create(USER_CODE_VA, USER_STACK_TOP, 0);
    if (!process || process_set_current(process) != 0) {
        page_free(code);
        page_free(stack);
        return -1;
    }

    enter_user_mode(USER_CODE_VA, USER_STACK_TOP);
}
