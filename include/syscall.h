#ifndef LIONOS_SYSCALL_H
#define LIONOS_SYSCALL_H

#include <stdint.h>

#define SYS_ABI_VERSION 0u
#define SYS_PUTC        1u
#define SYS_GETPID      2u
#define SYS_YIELD       3u
#define SYS_EXIT        4u
#define SYS_GETCHAR     5u
#define SYS_KBD_AVAIL   6u
#define SYS_WRITE       7u
#define SYS_READ        8u
#define SYS_CLEAR       9u
#define SYS_MEMINFO     10u

#define SYSCALL_OK 0u
#define SYSCALL_ERR ((uint32_t)-1)

uint32_t syscall_handle(uint32_t number, uint32_t arg0, uint32_t arg1, uint32_t arg2);
void syscall_init(void);

#endif
