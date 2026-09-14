#ifndef LIONOS_SYSCALL_H
#define LIONOS_SYSCALL_H

#include <stdint.h>
#include "uapi.h"

#define SYS_ABI_VERSION LIONOS_SYSCALL_ABI
#define SYS_PUTC        LIONOS_SYS_PUTC
#define SYS_GETPID      LIONOS_SYS_GETPID
#define SYS_YIELD       LIONOS_SYS_YIELD
#define SYS_EXIT        LIONOS_SYS_EXIT
#define SYS_GETCHAR     LIONOS_SYS_GETCHAR
#define SYS_KBD_AVAIL   LIONOS_SYS_KBD_AVAIL
#define SYS_WRITE       LIONOS_SYS_WRITE
#define SYS_READ        LIONOS_SYS_READ
#define SYS_CLEAR       LIONOS_SYS_CLEAR
#define SYS_MEMINFO     LIONOS_SYS_MEMINFO
#define SYS_EXEC        LIONOS_SYS_EXEC

#define SYSCALL_OK LIONOS_SYSCALL_OK
#define SYSCALL_ERR LIONOS_SYSCALL_ERR

uint32_t syscall_handle(uint32_t number, uint32_t arg0, uint32_t arg1, uint32_t arg2);
void syscall_init(void);

#endif
