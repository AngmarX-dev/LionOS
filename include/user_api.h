#ifndef LIONOS_USER_API_H
#define LIONOS_USER_API_H

#include <stdint.h>
#include "uapi.h"

static inline uint32_t lion_syscall0(uint32_t n){uint32_t r;__asm__ volatile("int $0x80":"=a"(r):"a"(n):"ebx","ecx","edx","memory");return r;}
static inline uint32_t lion_syscall1(uint32_t n,uint32_t a){uint32_t r;__asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a):"ecx","edx","memory");return r;}
static inline uint32_t lion_syscall2(uint32_t n,uint32_t a,uint32_t b){uint32_t r;__asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b):"edx","memory");return r;}
static inline uint32_t lion_syscall3(uint32_t n,uint32_t a,uint32_t b,uint32_t c){uint32_t r;__asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c):"memory");return r;}
static inline uint32_t lion_putc(char c){return lion_syscall1(LIONOS_SYS_PUTC,(uint32_t)(uint8_t)c);}
static inline uint32_t lion_getpid(void){return lion_syscall0(LIONOS_SYS_GETPID);}
static inline uint32_t lion_getppid(void){return lion_syscall0(LIONOS_SYS_GETPPID);}
static inline uint32_t lion_yield(void){return lion_syscall0(LIONOS_SYS_YIELD);}
static inline void lion_exit_code(uint32_t c){(void)lion_syscall1(LIONOS_SYS_EXIT,c);for(;;)__asm__ volatile("hlt");}
static inline void lion_exit(void){lion_exit_code(0);}
static inline uint32_t lion_write(const char*s,uint32_t n){return lion_syscall2(LIONOS_SYS_WRITE,(uint32_t)(uintptr_t)s,n);}
static inline uint32_t lion_read(char*b,uint32_t n){return lion_syscall2(LIONOS_SYS_READ,(uint32_t)(uintptr_t)b,n);}
static inline uint32_t lion_clear(void){return lion_syscall0(LIONOS_SYS_CLEAR);}
static inline uint32_t lion_meminfo(void){return lion_syscall0(LIONOS_SYS_MEMINFO);}
static inline uint32_t lion_exec(const char*n){return lion_syscall1(LIONOS_SYS_EXEC,(uint32_t)(uintptr_t)n);}
static inline uint32_t lion_fork(void){return lion_syscall0(LIONOS_SYS_FORK);}
static inline int32_t lion_waitpid(uint32_t p,int32_t*s){return(int32_t)lion_syscall2(LIONOS_SYS_WAITPID,p,(uint32_t)(uintptr_t)s);}
static inline int32_t lion_open(const char*p,uint32_t f){return(int32_t)lion_syscall2(LIONOS_SYS_OPEN,(uint32_t)(uintptr_t)p,f);}
static inline int32_t lion_close(int32_t fd){return(int32_t)lion_syscall1(LIONOS_SYS_CLOSE,(uint32_t)fd);}
static inline int32_t lion_fread(int32_t fd,void*b,uint32_t n){return(int32_t)lion_syscall3(LIONOS_SYS_FREAD,(uint32_t)fd,(uint32_t)(uintptr_t)b,n);}
static inline int32_t lion_fwrite(int32_t fd,const void*b,uint32_t n){return(int32_t)lion_syscall3(LIONOS_SYS_FWRITE,(uint32_t)fd,(uint32_t)(uintptr_t)b,n);}
static inline int32_t lion_remove(const char*p){return(int32_t)lion_syscall1(LIONOS_SYS_REMOVE,(uint32_t)(uintptr_t)p);}
static inline int32_t lion_stat(const char*p,struct lion_stat*s){return(int32_t)lion_syscall2(LIONOS_SYS_STAT,(uint32_t)(uintptr_t)p,(uint32_t)(uintptr_t)s);}
static inline int32_t lion_ipc_send(uint32_t pid,const void*data,uint32_t length){return(int32_t)lion_syscall3(LIONOS_SYS_IPC_SEND,pid,(uint32_t)(uintptr_t)data,length);}
static inline int32_t lion_ipc_recv(void*data,uint32_t capacity,uint32_t*sender_pid){return(int32_t)lion_syscall3(LIONOS_SYS_IPC_RECV,(uint32_t)(uintptr_t)data,capacity,(uint32_t)(uintptr_t)sender_pid);}
static inline uint32_t lion_ipc_pending(void){return lion_syscall0(LIONOS_SYS_IPC_PENDING);}
static inline int32_t lion_kill(uint32_t pid,uint32_t signal){return(int32_t)lion_syscall2(LIONOS_SYS_KILL,pid,signal);}
static inline int32_t lion_getstate(uint32_t pid){return(int32_t)lion_syscall1(LIONOS_SYS_GETSTATE,pid);}
static inline uint32_t lion_sigpending(uint32_t pid){return lion_syscall1(LIONOS_SYS_SIGPENDING,pid);}
static inline int32_t lion_net_send(uint32_t ip,uint16_t src_port,uint16_t dst_port,const void*data,uint32_t length){return(int32_t)lion_syscall3(LIONOS_SYS_NET_SEND,ip,((uint32_t)src_port<<16)|dst_port,(uint32_t)(uintptr_t)data);}
static inline int32_t lion_net_recv(uint16_t port,void*data,uint32_t capacity,uint32_t*src_ip,uint16_t*src_port){return(int32_t)lion_syscall3(LIONOS_SYS_NET_RECV,port,(uint32_t)(uintptr_t)data,(capacity&0xFFFFu)|((uint32_t)(uintptr_t)src_ip<<16));}
static inline uint32_t lion_net_pending(uint16_t port){return lion_syscall1(LIONOS_SYS_NET_PENDING,port);}
static inline uint32_t lion_net_getip(void){return lion_syscall0(LIONOS_SYS_NET_GETIP);}

#endif
