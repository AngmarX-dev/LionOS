#ifndef LIONOS_USER_API_H
#define LIONOS_USER_API_H

#include <stdint.h>
#include "uapi.h"

#define LIONOS_PROCESS_READY 1u
#define LIONOS_PROCESS_ZOMBIE 3u
#define LIONOS_PROCESS_STOPPED 5u

#define LIONOS_SIG_NONE 0u
#define LIONOS_SIG_TERM 15u
#define LIONOS_SIG_KILL 9u
#define LIONOS_SIG_STOP 19u
#define LIONOS_SIG_CONT 18u
#define LIONOS_SIG_MAX 31u

static inline uint32_t lion_syscall0(uint32_t n){uint32_t r;__asm__ volatile("int $0x80":"=a"(r):"a"(n):"ebx","ecx","edx","esi","edi","memory");return r;}
static inline uint32_t lion_syscall1(uint32_t n,uint32_t a){uint32_t r;__asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a):"ecx","edx","esi","edi","memory");return r;}
static inline uint32_t lion_syscall2(uint32_t n,uint32_t a,uint32_t b){uint32_t r;__asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b):"edx","esi","edi","memory");return r;}
static inline uint32_t lion_syscall3(uint32_t n,uint32_t a,uint32_t b,uint32_t c){uint32_t r;__asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c):"esi","edi","memory");return r;}
static inline uint32_t lion_syscall4(uint32_t n,uint32_t a,uint32_t b,uint32_t c,uint32_t d){uint32_t r;__asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c),"S"(d):"edi","memory");return r;}
static inline uint32_t lion_syscall5(uint32_t n,uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e){uint32_t r;__asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c),"S"(d),"D"(e):"memory");return r;}
static inline uint32_t lion_mouse_read(struct lion_mouse_event *event){return lion_syscall1(LIONOS_SYS_MOUSE_READ,(uint32_t)(uintptr_t)event);}
static inline uint32_t lion_mouse_pending(void){return lion_syscall0(LIONOS_SYS_MOUSE_PENDING);}
static inline uint32_t lion_putc(char c){return lion_syscall1(LIONOS_SYS_PUTC,(uint32_t)(uint8_t)c);}
static inline uint32_t lion_getpid(void){return lion_syscall0(LIONOS_SYS_GETPID);}
static inline uint32_t lion_gettid(void){return lion_syscall0(LIONOS_SYS_GETTID);}
static inline uint32_t lion_thread_create(void(*entry)(void)){return lion_syscall1(LIONOS_SYS_THREAD_CREATE,(uint32_t)(uintptr_t)entry);}
static inline int32_t lion_thread_join(uint32_t tid,int32_t*status){return(int32_t)lion_syscall2(LIONOS_SYS_THREAD_JOIN,tid,(uint32_t)(uintptr_t)status);}
static inline void lion_thread_exit(uint32_t code){(void)lion_syscall1(LIONOS_SYS_THREAD_EXIT,code);for(;;)__asm__ volatile("cli; hlt");}
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
static inline int32_t lion_readlink(const char*p,char*b,uint32_t n){return(int32_t)lion_syscall3(LIONOS_SYS_READLINK,(uint32_t)(uintptr_t)p,(uint32_t)(uintptr_t)b,n);}
static inline int32_t lion_lstat(const char*p,struct lion_stat*s){return(int32_t)lion_syscall2(LIONOS_SYS_LSTAT,(uint32_t)(uintptr_t)p,(uint32_t)(uintptr_t)s);}
static inline int32_t lion_sys_readdir(const char*p,uint32_t*offset,struct lion_dirent*entry){return(int32_t)lion_syscall3(LIONOS_SYS_READDIR,(uint32_t)(uintptr_t)p,(uint32_t)(uintptr_t)offset,(uint32_t)(uintptr_t)entry);}
static inline int32_t lion_getfile(uint32_t index,char*name,uint32_t capacity){return(int32_t)lion_syscall3(LIONOS_SYS_GETFILE,index,(uint32_t)(uintptr_t)name,capacity);}
static inline int32_t lion_ipc_send(uint32_t pid,const void*data,uint32_t length){return(int32_t)lion_syscall3(LIONOS_SYS_IPC_SEND,pid,(uint32_t)(uintptr_t)data,length);}
static inline int32_t lion_ipc_recv(void*data,uint32_t capacity,uint32_t*sender_pid){return(int32_t)lion_syscall3(LIONOS_SYS_IPC_RECV,(uint32_t)(uintptr_t)data,capacity,(uint32_t)(uintptr_t)sender_pid);}
static inline uint32_t lion_ipc_pending(void){return lion_syscall0(LIONOS_SYS_IPC_PENDING);}
static inline int32_t lion_ipc_recv_blocking(void*data,uint32_t capacity,uint32_t*sender){return(int32_t)lion_syscall3(LIONOS_SYS_IPC_RECV_BLOCKING,(uint32_t)(uintptr_t)data,capacity,(uint32_t)(uintptr_t)sender);}
static inline int32_t lion_pipe_create(uint32_t*read_end,uint32_t*write_end){return(int32_t)lion_syscall2(LIONOS_SYS_PIPE_CREATE,(uint32_t)(uintptr_t)read_end,(uint32_t)(uintptr_t)write_end);}
static inline int32_t lion_pipe_read(uint32_t handle,void*data,uint32_t capacity){return(int32_t)lion_syscall3(LIONOS_SYS_PIPE_READ,handle,(uint32_t)(uintptr_t)data,capacity);}
static inline int32_t lion_pipe_write(uint32_t handle,const void*data,uint32_t length){return(int32_t)lion_syscall3(LIONOS_SYS_PIPE_WRITE,handle,(uint32_t)(uintptr_t)data,length);}
static inline int32_t lion_pipe_close(uint32_t handle){return(int32_t)lion_syscall1(LIONOS_SYS_PIPE_CLOSE,handle);}
static inline int32_t lion_kill(uint32_t pid,uint32_t signal){return(int32_t)lion_syscall2(LIONOS_SYS_KILL,pid,signal);}
static inline int32_t lion_getstate(uint32_t pid){return(int32_t)lion_syscall1(LIONOS_SYS_GETSTATE,pid);}
static inline uint32_t lion_sigpending(uint32_t pid){return lion_syscall1(LIONOS_SYS_SIGPENDING,pid);}
static inline int32_t lion_net_send(uint32_t ip,uint16_t src_port,uint16_t dst_port,const void*data,uint32_t length){return(int32_t)lion_syscall4(LIONOS_SYS_NET_SEND,ip,((uint32_t)src_port<<16)|dst_port,(uint32_t)(uintptr_t)data,length);}
static inline int32_t lion_net_recv(uint16_t port,void*data,uint32_t capacity,uint32_t*src_ip,uint16_t*src_port){return(int32_t)lion_syscall5(LIONOS_SYS_NET_RECV,port,(uint32_t)(uintptr_t)data,capacity,(uint32_t)(uintptr_t)src_ip,(uint32_t)(uintptr_t)src_port);}
static inline uint32_t lion_net_pending(uint16_t port){return lion_syscall1(LIONOS_SYS_NET_PENDING,port);}
static inline uint32_t lion_net_getip(void){return lion_syscall0(LIONOS_SYS_NET_GETIP);}
static inline int32_t lion_net_ping(uint32_t ip){return (int32_t)lion_syscall1(LIONOS_SYS_NET_PING,ip);}


/*
 * Blocking userspace synchronization primitives for threads sharing an
 * address space. Futex waits always re-check the atomic predicate.
 */
typedef struct lion_mutex {
    volatile uint32_t state;
} lion_mutex_t;

typedef struct lion_semaphore {
    volatile uint32_t count;
} lion_semaphore_t;

static inline int lion_futex_wait_timeout(volatile uint32_t *word, uint32_t expected,
                                          uint32_t timeout_ticks) {
    if (!word || ((uintptr_t)word & 3u) || timeout_ticks > 0x7FFFFFFFu) return -1;
    return (int32_t)lion_syscall3(LIONOS_SYS_FUTEX_WAIT_TIMEOUT,
                                  (uint32_t)(uintptr_t)word, expected, timeout_ticks);
}

static inline int lion_futex_wait(volatile uint32_t *word, uint32_t expected) {
    return lion_futex_wait_timeout(word, expected, 0u);
}

static inline int lion_futex_wake(volatile uint32_t *word, uint32_t maximum) {
    if (!word || ((uintptr_t)word & 3u)) return -1;
    return (int32_t)lion_syscall2(LIONOS_SYS_FUTEX_WAKE,
                                  (uint32_t)(uintptr_t)word, maximum);
}

static inline int lion_mutex_init(lion_mutex_t *mutex) {
    if (!mutex || ((uintptr_t)mutex & 3u)) return -1;
    __atomic_store_n(&mutex->state, 0u, __ATOMIC_RELEASE);
    return 0;
}

static inline int lion_mutex_trylock(lion_mutex_t *mutex) {
    if (!mutex || ((uintptr_t)mutex & 3u)) return -1;
    return __sync_val_compare_and_swap(&mutex->state, 0u, 1u) == 0u ? 0 : 1;
}

static inline int lion_mutex_lock(lion_mutex_t *mutex) {
    if (!mutex || ((uintptr_t)mutex & 3u)) return -1;
    for (;;) {
        if (__sync_val_compare_and_swap(&mutex->state, 0u, 1u) == 0u) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            return 0;
        }
        int result = lion_futex_wait(&mutex->state, 1u);
        if (result < 0 && result != -2) return -1;
    }
}

static inline int lion_mutex_unlock(lion_mutex_t *mutex) {
    if (!mutex || ((uintptr_t)mutex & 3u)) return -1;
    if (__sync_val_compare_and_swap(&mutex->state, 1u, 0u) != 1u) return -1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return lion_futex_wake(&mutex->state, 1u) < 0 ? -1 : 0;
}

static inline int lion_semaphore_init(lion_semaphore_t *sem, uint32_t initial) {
    if (!sem || ((uintptr_t)sem & 3u)) return -1;
    __atomic_store_n(&sem->count, initial, __ATOMIC_RELEASE);
    return 0;
}

static inline int lion_semaphore_wait(lion_semaphore_t *sem) {
    if (!sem || ((uintptr_t)sem & 3u)) return -1;
    for (;;) {
        uint32_t observed = __atomic_load_n(&sem->count, __ATOMIC_ACQUIRE);
        while (observed != 0u) {
            if (__sync_val_compare_and_swap(&sem->count, observed, observed - 1u) == observed)
                return 0;
            observed = __atomic_load_n(&sem->count, __ATOMIC_ACQUIRE);
        }
        int result = lion_futex_wait(&sem->count, 0u);
        if (result < 0 && result != -2) return -1;
    }
}

static inline int lion_semaphore_post(lion_semaphore_t *sem) {
    if (!sem || ((uintptr_t)sem & 3u)) return -1;
    uint32_t observed = __atomic_load_n(&sem->count, __ATOMIC_RELAXED);
    for (;;) {
        if (observed == UINT32_MAX) return -1;
        uint32_t previous = __sync_val_compare_and_swap(&sem->count, observed, observed + 1u);
        if (previous == observed) break;
        observed = previous;
    }
    return lion_futex_wake(&sem->count, 1u) < 0 ? -1 : 0;
}

#endif
