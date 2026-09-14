#include <stdint.h>
#include "console.h"
#include "exec.h"
#include "keyboard.h"
#include "memory.h"
#include "process.h"
#include "syscall.h"

#define USER_MIN 0x00400000u
#define USER_MAX 0xC0000000u
#define USER_COPY_MAX 4096u

static int user_range_ok(uint32_t ptr, uint32_t len) {
    if (len == 0) return ptr >= USER_MIN && ptr < USER_MAX;
    if (ptr < USER_MIN || ptr >= USER_MAX) return 0;
    return len <= USER_MAX - ptr;
}

static int copy_user_string(char *dst, uint32_t dst_size, uint32_t user_ptr) {
    if (!dst || dst_size < 2u || !user_range_ok(user_ptr, 1u)) return -1;
    for (uint32_t i = 0; i < dst_size - 1u; ++i) {
        uint32_t address = user_ptr + i;
        if (!user_range_ok(address, 1u)) return -1;
        dst[i] = *(const char *)(uintptr_t)address;
        if (dst[i] == 0) return 0;
    }
    dst[dst_size - 1u] = 0;
    return -1;
}

static uint32_t syscall_dispatch(uint32_t number, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    (void)arg2;
    switch (number) {
        case SYS_ABI_VERSION: return 1;
        case SYS_PUTC:
            if (arg0 > 0xFFu) return SYSCALL_ERR;
            console_putc((char)arg0); return SYSCALL_OK;
        case SYS_GETPID: return process_current_pid();
        case SYS_YIELD: __asm__ volatile ("pause"); return SYSCALL_OK;
        case SYS_EXIT: process_exit_current(); return SYSCALL_OK;
        case SYS_GETCHAR: {
            int c = keyboard_getchar();
            return c < 0 ? SYSCALL_ERR : (uint32_t)(uint8_t)c;
        }
        case SYS_KBD_AVAIL: return keyboard_available();
        case SYS_WRITE:
            if (arg1 > USER_COPY_MAX || !user_range_ok(arg0, arg1)) return SYSCALL_ERR;
            console_write_n((const char *)(uintptr_t)arg0, arg1);
            return arg1;
        case SYS_READ: {
            if (arg1 > USER_COPY_MAX || !user_range_ok(arg0, arg1)) return SYSCALL_ERR;
            uint32_t n = 0;
            while (n < arg1) {
                int c = keyboard_getchar();
                if (c < 0) break;
                ((char *)(uintptr_t)arg0)[n++] = (char)c;
            }
            return n;
        }
        case SYS_CLEAR: console_clear(); return SYSCALL_OK;
        case SYS_MEMINFO: return memory_free_pages();
        case SYS_EXEC: {
            char name[64];
            if (copy_user_string(name, sizeof(name), arg0) != 0) return SYSCALL_ERR;
            int pid = exec_run_file(name);
            if (pid < 0) return SYSCALL_ERR;
            process_exit_current();
            return (uint32_t)pid;
        }
        case SYS_FORK: {
            struct process *parent = process_current();
            uint32_t pid = process_fork_current((uint32_t *)(uintptr_t)arg0);
            (void)parent;
            return pid ? pid : SYSCALL_ERR;
        }
        default: return SYSCALL_ERR;
    }
}

void syscall_init(void) { (void)syscall_dispatch; }
uint32_t syscall_handle(uint32_t number, uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    return syscall_dispatch(number, arg0, arg1, arg2);
}
