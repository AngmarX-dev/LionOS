#ifndef LIONOS_USER_API_H
#define LIONOS_USER_API_H

#include <stdint.h>
#include "uapi.h"

static inline uint32_t lion_syscall0(uint32_t number) {
    uint32_t result;
    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(number) : "ebx", "ecx", "edx", "memory");
    return result;
}

static inline uint32_t lion_syscall1(uint32_t number, uint32_t arg0) {
    uint32_t result;
    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(number), "b"(arg0) : "ecx", "edx", "memory");
    return result;
}

static inline uint32_t lion_syscall2(uint32_t number, uint32_t arg0, uint32_t arg1) {
    uint32_t result;
    __asm__ volatile ("int $0x80" : "=a"(result) : "a"(number), "b"(arg0), "c"(arg1) : "edx", "memory");
    return result;
}

static inline uint32_t lion_putc(char c) { return lion_syscall1(LIONOS_SYS_PUTC, (uint32_t)(uint8_t)c); }
static inline uint32_t lion_getpid(void) { return lion_syscall0(LIONOS_SYS_GETPID); }
static inline uint32_t lion_yield(void) { return lion_syscall0(LIONOS_SYS_YIELD); }
static inline void lion_exit(void) { (void)lion_syscall0(LIONOS_SYS_EXIT); for (;;) __asm__ volatile ("hlt"); }
static inline uint32_t lion_write(const char *s, uint32_t length) { return lion_syscall2(LIONOS_SYS_WRITE, (uint32_t)(uintptr_t)s, length); }
static inline uint32_t lion_read(char *buffer, uint32_t length) { return lion_syscall2(LIONOS_SYS_READ, (uint32_t)(uintptr_t)buffer, length); }
static inline uint32_t lion_clear(void) { return lion_syscall0(LIONOS_SYS_CLEAR); }
static inline uint32_t lion_meminfo(void) { return lion_syscall0(LIONOS_SYS_MEMINFO); }
static inline uint32_t lion_exec(const char *name) { return lion_syscall1(LIONOS_SYS_EXEC, (uint32_t)(uintptr_t)name); }
static inline uint32_t lion_fork(void) { return lion_syscall1(LIONOS_SYS_FORK, 0u); }

#endif
