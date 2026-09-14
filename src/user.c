#include <stdint.h>
#include "memory.h"
#include "paging.h"
#include "process.h"
#include "syscall.h"
#include "tss.h"
#include "user.h"

#define USER1_CODE_VA   0x00400000u
#define USER1_STACK_VA  0x00401000u
#define USER1_STACK_TOP 0x00402000u
#define USER2_CODE_VA   0x00402000u
#define USER2_STACK_VA  0x00403000u
#define USER2_STACK_TOP 0x00404000u

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

static const uint8_t program1[] = {
    0xB8, SYS_PUTC, 0x00, 0x00, 0x00,
    0xBB, 'A', 0x00, 0x00, 0x00,
    0xCD, 0x80,
    0xB8, SYS_YIELD, 0x00, 0x00, 0x00,
    0xCD, 0x80,
    0xB8, SYS_PUTC, 0x00, 0x00, 0x00,
    0xBB, 'a', 0x00, 0x00, 0x00,
    0xCD, 0x80,
    0xB8, SYS_EXIT, 0x00, 0x00, 0x00,
    0xCD, 0x80,
    0xF4,
    0xEB, 0xFC
};

static const uint8_t program2[] = {
    0xB8, SYS_PUTC, 0x00, 0x00, 0x00,
    0xBB, 'B', 0x00, 0x00, 0x00,
    0xCD, 0x80,
    0xB8, SYS_YIELD, 0x00, 0x00, 0x00,
    0xCD, 0x80,
    0xB8, SYS_PUTC, 0x00, 0x00, 0x00,
    0xBB, 'b', 0x00, 0x00, 0x00,
    0xCD, 0x80,
    0xB8, SYS_EXIT, 0x00, 0x00, 0x00,
    0xCD, 0x80,
    0xF4,
    0xEB, 0xFC
};

static int setup_user_process(uint8_t *code, uint8_t *stack,
                              const uint8_t *program, uint32_t code_va,
                              uint32_t stack_va, uint32_t stack_top,
                              struct process **out_process) {
    for (uint32_t i = 0; i < 4096u; ++i) stack[i] = 0;
    for (uint32_t i = 0; i < 4096u; ++i) code[i] = 0x90u;
    for (uint32_t i = 0; i < 128u && program[i] != 0; ++i) code[i] = program[i];

    if (paging_map_user_page(code_va, (uint32_t)code, 0x5u) != 0 ||
        paging_map_user_page(stack_va, (uint32_t)stack, 0x7u) != 0) {
        return -1;
    }

    *out_process = process_create(code_va, stack_top, 0);
    if (!*out_process) return -1;
    return 0;
}

int user_mode_test(void) {
    uint8_t *code1 = (uint8_t *)page_alloc();
    uint8_t *stack1 = (uint8_t *)page_alloc();
    uint8_t *code2 = (uint8_t *)page_alloc();
    uint8_t *stack2 = (uint8_t *)page_alloc();

    if (!code1 || !stack1 || !code2 || !stack2) {
        if (code1) page_free(code1);
        if (stack1) page_free(stack1);
        if (code2) page_free(code2);
        if (stack2) page_free(stack2);
        return -1;
    }

    struct process *process1 = 0;
    struct process *process2 = 0;

    if (setup_user_process(code1, stack1, program1, USER1_CODE_VA,
                           USER1_STACK_VA, USER1_STACK_TOP, &process1) != 0 ||
        setup_user_process(code2, stack2, program2, USER2_CODE_VA,
                           USER2_STACK_VA, USER2_STACK_TOP, &process2) != 0) {
        page_free(code1);
        page_free(stack1);
        page_free(code2);
        page_free(stack2);
        return -1;
    }

    if (process_set_current(process1) != 0) return -1;
    tss_set_kernel_stack(process_kernel_stack_top(process1));

    (void)process2;
    enter_user_mode(USER1_CODE_VA, USER1_STACK_TOP);
}
