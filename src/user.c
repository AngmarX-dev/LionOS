#include <stdint.h>
#include "memory.h"
#include "paging.h"
#include "user.h"

#define USER_CODE_VA  0x00400000u
#define USER_STACK_VA 0x00401000u
#define USER_STACK_TOP 0x00402000u
#define USER_CODE_SEL 0x23u
#define USER_DATA_SEL 0x2Bu

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
        "iretd\n"
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

    /* mov eax,1; mov ebx,42; int 0x80; jmp $-2 */
    static const uint8_t program[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00,
        0xBB, 0x2A, 0x00, 0x00, 0x00,
        0xCD, 0x80,
        0xEB, 0xFE
    };

    for (uint32_t i = 0; i < sizeof(program); ++i) code[i] = program[i];
    for (uint32_t i = 0; i < 4096u; ++i) stack[i] = 0;

    if (paging_map_user_page(USER_CODE_VA, (uint32_t)code, 0x5u) != 0 ||
        paging_map_user_page(USER_STACK_VA, (uint32_t)stack, 0x7u) != 0) {
        page_free(code);
        page_free(stack);
        return -1;
    }

    enter_user_mode(USER_CODE_VA, USER_STACK_TOP);
}
