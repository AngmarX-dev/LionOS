#ifndef LIONOS_UAPI_H
#define LIONOS_UAPI_H

#include <stdint.h>
#include "vfs_uapi.h"
#include "ipc.h"
#include "net.h"

#define LIONOS_UAPI_VERSION 4u
#define LIONOS_SYSCALL_ABI 4u
#define LIONOS_SYS_PUTC 1u
#define LIONOS_SYS_GETPID 2u
#define LIONOS_SYS_YIELD 3u
#define LIONOS_SYS_EXIT 4u
#define LIONOS_SYS_GETCHAR 5u
#define LIONOS_SYS_KBD_AVAIL 6u
#define LIONOS_SYS_WRITE 7u
#define LIONOS_SYS_READ 8u
#define LIONOS_SYS_CLEAR 9u
#define LIONOS_SYS_MEMINFO 10u
#define LIONOS_SYS_EXEC 11u
#define LIONOS_SYS_FORK 12u
#define LIONOS_SYS_WAITPID 13u
#define LIONOS_SYS_IPC_SEND 20u
#define LIONOS_SYS_IPC_RECV 21u
#define LIONOS_SYS_IPC_PENDING 22u
#define LIONOS_SYS_GETPPID 23u
#define LIONOS_SYS_KILL 24u
#define LIONOS_SYS_GETSTATE 25u
#define LIONOS_SYS_SIGPENDING 26u
#define LIONOS_SYS_NET_SEND 27u
#define LIONOS_SYS_NET_RECV 28u
#define LIONOS_SYS_NET_PENDING 29u
#define LIONOS_SYS_NET_GETIP 30u

#define LIONOS_SYSCALL_OK 0u
#define LIONOS_SYSCALL_ERR ((uint32_t)-1)
#define LIONOS_IPC_EMPTY ((uint32_t)-2)
#define LIONOS_NET_EMPTY ((uint32_t)-2)

#endif
