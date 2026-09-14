#include <stdint.h>
#include "console.h"
#include "exec.h"
#include "ipc.h"
#include "keyboard.h"
#include "memory.h"
#include "net.h"
#include "process.h"
#include "signal.h"
#include "syscall.h"
#include "vfs.h"
#include "vfs_uapi.h"

#define USER_MIN 0x00400000u
#define USER_MAX 0xC0000000u
#define USER_COPY_MAX 4096u
#define PROCESS_WAIT_BLOCKED ((uint32_t)-2)

static int user_range_ok(uint32_t ptr,uint32_t len){if(len==0)return ptr>=USER_MIN&&ptr<USER_MAX;if(ptr<USER_MIN||ptr>=USER_MAX)return 0;return len<=USER_MAX-ptr;}
static int copy_user_string(char*dst,uint32_t dst_size,uint32_t user_ptr){if(!dst||dst_size<2u||!user_range_ok(user_ptr,1u))return-1;for(uint32_t i=0;i<dst_size-1u;++i){uint32_t a=user_ptr+i;if(!user_range_ok(a,1u))return-1;dst[i]=*(const char*)(uintptr_t)a;if(!dst[i])return 0;}dst[dst_size-1u]=0;return-1;}
static int process_exists(uint32_t pid){if(!pid)return 0;for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i){struct process*p=process_at(i);if(p&&p->state!=PROCESS_UNUSED&&p->pid==pid)return 1;}return 0;}

static uint32_t syscall_dispatch(uint32_t number,uint32_t arg0,uint32_t arg1,uint32_t arg2,uint32_t arg3,uint32_t arg4){switch(number){
case SYS_ABI_VERSION:return 4u;
case SYS_PUTC:if(arg0>0xFFu)return SYSCALL_ERR;console_putc((char)arg0);return SYSCALL_OK;
case SYS_GETPID:return process_current_pid();
case SYS_YIELD:__asm__ volatile("pause");return SYSCALL_OK;
case SYS_EXIT:process_exit_current(arg0);return SYSCALL_OK;
case SYS_GETCHAR:{int c=keyboard_getchar();return c<0?SYSCALL_ERR:(uint32_t)(uint8_t)c;}
case SYS_KBD_AVAIL:return keyboard_available();
case SYS_WRITE:if(arg1>USER_COPY_MAX||!user_range_ok(arg0,arg1))return SYSCALL_ERR;console_write_n((const char*)(uintptr_t)arg0,arg1);return arg1;
case SYS_READ:{if(arg1>USER_COPY_MAX||!user_range_ok(arg0,arg1))return SYSCALL_ERR;uint32_t n=0;while(n<arg1){int c=keyboard_getchar();if(c<0)break;((char*)(uintptr_t)arg0)[n++]=(char)c;}return n;}
case SYS_CLEAR:console_clear();return SYSCALL_OK;
case SYS_MEMINFO:return memory_free_pages();
case SYS_EXEC:{char name[64];if(copy_user_string(name,sizeof(name),arg0)!=0)return SYSCALL_ERR;int pid=exec_replace_current(name);return pid<0?SYSCALL_ERR:(uint32_t)pid;}
case SYS_FORK:return process_fork_current(0);
case SYS_WAITPID:if(!user_range_ok(arg1,sizeof(uint32_t)))return SYSCALL_ERR;{int32_t result=process_waitpid(arg0,arg1);return result==PROCESS_WAIT_BLOCKED?PROCESS_WAIT_BLOCKED:(uint32_t)result;}
case LIONOS_SYS_OPEN:{char path[VFS_PATH_MAX];if(copy_user_string(path,sizeof(path),arg0)!=0)return SYSCALL_ERR;return(uint32_t)vfs_open(path,arg1);}
case LIONOS_SYS_CLOSE:return(uint32_t)vfs_close((int)arg0);
case LIONOS_SYS_FREAD:if(arg2>USER_COPY_MAX||!user_range_ok(arg1,arg2))return SYSCALL_ERR;return(uint32_t)vfs_read((int)arg0,(void*)(uintptr_t)arg1,arg2);
case LIONOS_SYS_FWRITE:if(arg2>USER_COPY_MAX||!user_range_ok(arg1,arg2))return SYSCALL_ERR;return(uint32_t)vfs_write((int)arg0,(const void*)(uintptr_t)arg1,arg2);
case LIONOS_SYS_REMOVE:{char path[VFS_PATH_MAX];if(copy_user_string(path,sizeof(path),arg0)!=0)return SYSCALL_ERR;return(uint32_t)vfs_remove(path);}
case LIONOS_SYS_STAT:{char path[VFS_PATH_MAX];struct lion_stat*st=(struct lion_stat*)(uintptr_t)arg1;if(copy_user_string(path,sizeof(path),arg0)!=0||!user_range_ok(arg1,sizeof(*st)))return SYSCALL_ERR;struct vfs_stat kst;if(vfs_stat_path(path,&kst)<0)return SYSCALL_ERR;st->size=kst.size;st->backend=kst.backend;st->flags=kst.flags;return SYSCALL_OK;}
case LIONOS_SYS_GETFILE:{if(arg1==0||arg2<2u||!user_range_ok(arg1,arg2))return SYSCALL_ERR;const char*n=vfs_name(arg0);if(!n)return SYSCALL_ERR;uint32_t i=0;while(n[i]&&i<arg2-1u){((char*)(uintptr_t)arg1)[i]=n[i];++i;}((char*)(uintptr_t)arg1)[i]=0;return i;}
case LIONOS_SYS_IPC_SEND:if(!process_exists(arg0)||arg0==process_current_pid()||arg2==0||arg2>IPC_MESSAGE_MAX||!user_range_ok(arg1,arg2))return SYSCALL_ERR;return(uint32_t)ipc_send(arg0,process_current_pid(),(const void*)(uintptr_t)arg1,arg2);
case LIONOS_SYS_IPC_RECV:{if(arg1==0||arg1>IPC_MESSAGE_MAX||!user_range_ok(arg0,arg1))return SYSCALL_ERR;if(arg2&&!user_range_ok(arg2,sizeof(uint32_t)))return SYSCALL_ERR;uint32_t sender=0;int32_t n=ipc_recv(process_current_pid(),(void*)(uintptr_t)arg0,arg1,&sender);if(n==IPC_RECV_EMPTY)return LIONOS_IPC_EMPTY;if(n<0)return SYSCALL_ERR;if(arg2)*(uint32_t*)(uintptr_t)arg2=sender;return(uint32_t)n;}
case LIONOS_SYS_IPC_PENDING:return ipc_pending(process_current_pid());
case LIONOS_SYS_GETPPID:{struct process*p=process_current();return p?p->parent_pid:0u;}
case LIONOS_SYS_KILL:if(!process_exists(arg0)||arg0==process_current_pid()||arg1==0||arg1>LIONOS_SIG_MAX)return SYSCALL_ERR;return(uint32_t)process_signal(arg0,arg1);
case LIONOS_SYS_GETSTATE:return(uint32_t)process_get_state(arg0);
case LIONOS_SYS_SIGPENDING:return process_signal_pending(arg0);
case LIONOS_SYS_NET_SEND:if(arg2==0||arg3==0||arg3>NET_PACKET_MAX||!user_range_ok(arg2,arg3))return SYSCALL_ERR;return(uint32_t)net_send(arg0,(uint16_t)(arg1>>16),(uint16_t)arg1,(const void*)(uintptr_t)arg2,arg3);
case LIONOS_SYS_NET_RECV:{if(arg1==0||arg1>NET_PACKET_MAX||!user_range_ok(arg1,arg2))return SYSCALL_ERR;uint32_t sip=0;uint16_t sport=0;int32_t n=net_recv((uint16_t)arg0,(void*)(uintptr_t)arg1,arg2,&sip,&sport);if(n==NET_RECV_EMPTY)return LIONOS_NET_EMPTY;if(n<0)return SYSCALL_ERR;if(arg3&&user_range_ok(arg3,sizeof(uint32_t)))*(uint32_t*)(uintptr_t)arg3=sip;if(arg4&&user_range_ok(arg4,sizeof(uint16_t)))*(uint16_t*)(uintptr_t)arg4=sport;return(uint32_t)n;}
case LIONOS_SYS_NET_PENDING:return net_pending((uint16_t)arg0);
case LIONOS_SYS_NET_GETIP:return net_local_ip();
default:return SYSCALL_ERR;}}
void syscall_init(void){(void)syscall_dispatch;}
uint32_t syscall_handle(uint32_t number,uint32_t arg0,uint32_t arg1,uint32_t arg2,uint32_t arg3,uint32_t arg4){return syscall_dispatch(number,arg0,arg1,arg2,arg3,arg4);}
