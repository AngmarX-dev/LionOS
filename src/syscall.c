#include <stdint.h>
#include "console.h"
#include "exec.h"
#include "ipc.h"
#include "keyboard.h"
#include "heap.h"
#include "memory.h"
#include "mouse.h"
#include "usercopy.h"
#include "pipe.h"
#include "net.h"
#include "process.h"
#include "signal.h"
#include "syscall.h"
#include "vfs.h"
#include "vfs_uapi.h"

#define USER_COPY_MAX 4096u

static void *syscall_copy_in(uint32_t address,uint32_t length){
    if(!length||length>USER_COPY_MAX)return 0;
    void *buffer=kmalloc(length);
    if(!buffer)return 0;
    if(usercopy_copy_from(buffer,address,length)!=0){kfree(buffer);return 0;}
    return buffer;
}
static int syscall_copy_out(uint32_t address,const void *buffer,uint32_t length){
    return usercopy_copy_to(address,buffer,length);
}

static int process_exists(uint32_t pid){if(!pid)return 0;for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i){struct process*p=process_at(i);if(p&&p->state!=PROCESS_UNUSED&&p->pid==pid)return 1;}return 0;}
static int has_cap(uint32_t cap){return process_has_capability(process_current(),cap);}
static uint32_t syscall_dispatch(uint32_t number,uint32_t arg0,uint32_t arg1,uint32_t arg2,uint32_t arg3,uint32_t arg4){switch(number){
case SYS_ABI_VERSION:return 5u;
case SYS_PUTC:if(!has_cap(PROCESS_CAP_CONSOLE))return SYSCALL_ERR;if(arg0>0xFFu)return SYSCALL_ERR;console_putc((char)arg0);return SYSCALL_OK;
case SYS_GETPID:return process_current_pid();
case LIONOS_SYS_GETTID:return process_current_tid();
case SYS_YIELD:__asm__ volatile("pause");return SYSCALL_OK;
case SYS_EXIT:process_exit_current(arg0);return SYSCALL_OK;
case SYS_GETCHAR:if(!has_cap(PROCESS_CAP_CONSOLE))return SYSCALL_ERR;{int c=keyboard_getchar();return c<0?SYSCALL_ERR:(uint32_t)(uint8_t)c;}
case SYS_KBD_AVAIL:if(!has_cap(PROCESS_CAP_CONSOLE))return SYSCALL_ERR;return keyboard_available();
case SYS_MOUSE_PENDING:if(!has_cap(PROCESS_CAP_CONSOLE))return SYSCALL_ERR;return mouse_event_available();
case SYS_MOUSE_READ:if(!has_cap(PROCESS_CAP_CONSOLE))return SYSCALL_ERR;{
    struct lion_mouse_event out;
    struct mouse_event ev;
    if(!usercopy_validate_write(arg0,sizeof(out)))return SYSCALL_ERR;
    int r=mouse_read_event(&ev);
    if(r==-2)return LIONOS_MOUSE_BLOCKED;
    if(r!=0)return SYSCALL_ERR;
    out.dx=ev.dx;out.dy=ev.dy;out.buttons=ev.buttons;out.source=ev.source;out.reserved[0]=0;out.reserved[1]=0;
    return syscall_copy_out(arg0,&out,sizeof(out))==0?SYSCALL_OK:SYSCALL_ERR;
}
case SYS_WRITE:if(!has_cap(PROCESS_CAP_CONSOLE))return SYSCALL_ERR;if(arg1>USER_COPY_MAX||!usercopy_validate_read(arg0,arg1))return SYSCALL_ERR;{uint8_t chunk[128];uint32_t done=0;while(done<arg1){uint32_t n=arg1-done;if(n>sizeof(chunk))n=sizeof(chunk);if(usercopy_copy_from(chunk,arg0+done,n)!=0)return done?done:SYSCALL_ERR;console_write_n((const char*)chunk,n);done+=n;}return done;}
case SYS_READ:if(!has_cap(PROCESS_CAP_CONSOLE))return SYSCALL_ERR;{if(arg1>USER_COPY_MAX||!usercopy_validate_write(arg0,arg1))return SYSCALL_ERR;if(arg1==0u)return 0u;uint8_t *buffer=(uint8_t*)kmalloc(arg1);if(!buffer)return SYSCALL_ERR;int32_t n=keyboard_read_blocking(buffer,arg1);if(n>0&&syscall_copy_out(arg0,buffer,(uint32_t)n)!=0)n=-1;kfree(buffer);return (uint32_t)n;}
case SYS_CLEAR:if(!has_cap(PROCESS_CAP_CONSOLE))return SYSCALL_ERR;console_clear();return SYSCALL_OK;
case SYS_MEMINFO:return memory_free_pages();
case SYS_EXEC:if(!has_cap(PROCESS_CAP_PROCESS))return SYSCALL_ERR;{char name[64];if(usercopy_copy_string(name,sizeof(name),arg0)!=0)return SYSCALL_ERR;int pid=exec_replace_current(name);return pid<0?SYSCALL_ERR:(uint32_t)pid;}
case SYS_FORK:if(!has_cap(PROCESS_CAP_PROCESS))return SYSCALL_ERR;return process_fork_current(0);
case SYS_WAITPID:if(!has_cap(PROCESS_CAP_PROCESS))return SYSCALL_ERR;if(!usercopy_validate_write(arg1,sizeof(uint32_t)))return SYSCALL_ERR;{int32_t result=process_waitpid(arg0,arg1);return result==(int32_t)PROCESS_WAIT_BLOCKED?(uint32_t)PROCESS_WAIT_BLOCKED:(uint32_t)result;}
case LIONOS_SYS_OPEN:if(!has_cap(PROCESS_CAP_FS))return SYSCALL_ERR;{char path[VFS_PATH_MAX];struct process*p=process_fd_owner();if(usercopy_copy_string(path,sizeof(path),arg0)!=0||!p)return SYSCALL_ERR;return(uint32_t)vfs_open_for_process(p,path,arg1);}
case LIONOS_SYS_CLOSE:if(!has_cap(PROCESS_CAP_FS))return SYSCALL_ERR;{struct process*p=process_fd_owner();return(uint32_t)vfs_close_for_process(p,(int)arg0);}
case LIONOS_SYS_FREAD:if(!has_cap(PROCESS_CAP_FS))return SYSCALL_ERR;if(arg2>USER_COPY_MAX||!usercopy_validate_write(arg1,arg2))return SYSCALL_ERR;if(!arg2)return 0u;{void *buffer=kmalloc(arg2);if(!buffer)return SYSCALL_ERR;int r=vfs_read_for_process(process_fd_owner(),(int)arg0,buffer,arg2);if(r>0&&syscall_copy_out(arg1,buffer,(uint32_t)r)!=0)r=-1;kfree(buffer);return(uint32_t)r;}
case LIONOS_SYS_FWRITE:if(!has_cap(PROCESS_CAP_FS))return SYSCALL_ERR;if(arg2>USER_COPY_MAX||!usercopy_validate_read(arg1,arg2))return SYSCALL_ERR;if(!arg2)return 0u;{void *buffer=syscall_copy_in(arg1,arg2);if(!buffer)return SYSCALL_ERR;int r=vfs_write_for_process(process_fd_owner(),(int)arg0,buffer,arg2);kfree(buffer);return(uint32_t)r;}
case LIONOS_SYS_REMOVE:if(!has_cap(PROCESS_CAP_FS))return SYSCALL_ERR;{char path[VFS_PATH_MAX];if(usercopy_copy_string(path,sizeof(path),arg0)!=0)return SYSCALL_ERR;return(uint32_t)vfs_remove(path);}
case LIONOS_SYS_STAT:if(!has_cap(PROCESS_CAP_FS))return SYSCALL_ERR;{char path[VFS_PATH_MAX];struct lion_stat st;struct vfs_stat kst;if(usercopy_copy_string(path,sizeof(path),arg0)!=0||!usercopy_validate_write(arg1,sizeof(st))||vfs_stat_path(path,&kst)<0)return SYSCALL_ERR;st.size=kst.size;st.backend=kst.backend;st.flags=kst.flags;return syscall_copy_out(arg1,&st,sizeof(st))==0?SYSCALL_OK:SYSCALL_ERR;}
case LIONOS_SYS_READLINK:if(!has_cap(PROCESS_CAP_FS))return SYSCALL_ERR;{char path[VFS_PATH_MAX];if(usercopy_copy_string(path,sizeof(path),arg0)!=0||arg2==0u||arg2>USER_COPY_MAX||!usercopy_validate_write(arg1,arg2))return SYSCALL_ERR;char *buffer=(char*)kmalloc(arg2);if(!buffer)return SYSCALL_ERR;int r=vfs_readlink(path,buffer,arg2);if(r>0&&syscall_copy_out(arg1,buffer,(uint32_t)r)!=0)r=-1;kfree(buffer);return(uint32_t)r;}
case LIONOS_SYS_LSTAT:if(!has_cap(PROCESS_CAP_FS))return SYSCALL_ERR;{char path[VFS_PATH_MAX];struct lion_stat st;struct vfs_stat kst;if(usercopy_copy_string(path,sizeof(path),arg0)!=0||!usercopy_validate_write(arg1,sizeof(st))||vfs_lstat_path(path,&kst)<0)return SYSCALL_ERR;st.size=kst.size;st.backend=kst.backend;st.flags=kst.flags;return syscall_copy_out(arg1,&st,sizeof(st))==0?SYSCALL_OK:SYSCALL_ERR;}
case LIONOS_SYS_READDIR:if(!has_cap(PROCESS_CAP_FS))return SYSCALL_ERR;{char path[VFS_PATH_MAX];struct lion_dirent user_entry;struct vfs_dirent kernel_entry;uint32_t offset;if(usercopy_copy_string(path,sizeof(path),arg0)!=0||!usercopy_validate_write(arg1,sizeof(offset))||!usercopy_validate_write(arg2,sizeof(user_entry))||(arg1<arg2+sizeof(user_entry)&&arg2<arg1+sizeof(offset))||usercopy_copy_from(&offset,arg1,sizeof(offset))!=0)return SYSCALL_ERR;int32_t result=vfs_readdir(path,&offset,&kernel_entry);if(result<0)return SYSCALL_ERR;if(syscall_copy_out(arg1,&offset,sizeof(offset))!=0)return SYSCALL_ERR;if(result==0)return 0u;user_entry.inode=kernel_entry.inode;user_entry.type=kernel_entry.type;for(uint32_t i=0u;i<sizeof(user_entry.name);++i)user_entry.name[i]=kernel_entry.name[i];if(syscall_copy_out(arg2,&user_entry,sizeof(user_entry))!=0)return SYSCALL_ERR;return 1u;}
case LIONOS_SYS_GETFILE:if(!has_cap(PROCESS_CAP_FS))return SYSCALL_ERR;{if(arg1==0||arg2<2u||arg2>USER_COPY_MAX||!usercopy_validate_write(arg1,arg2))return SYSCALL_ERR;const char*n=vfs_name(arg0);if(!n)return SYSCALL_ERR;char *buffer=(char*)kmalloc(arg2);if(!buffer)return SYSCALL_ERR;uint32_t i=0;while(n[i]&&i<arg2-1u){buffer[i]=n[i];++i;}buffer[i]=0;int r=syscall_copy_out(arg1,buffer,i+1u);kfree(buffer);return r==0?(uint32_t)i:SYSCALL_ERR;}
case LIONOS_SYS_IPC_SEND:if(!has_cap(PROCESS_CAP_IPC))return SYSCALL_ERR;if(!process_exists(arg0)||arg0==process_current_pid()||arg2==0||arg2>IPC_MESSAGE_MAX||!usercopy_validate_read(arg1,arg2))return SYSCALL_ERR;{void *buffer=syscall_copy_in(arg1,arg2);if(!buffer)return SYSCALL_ERR;int32_t r=ipc_send(arg0,process_current_pid(),buffer,arg2);kfree(buffer);return(uint32_t)r;}
case LIONOS_SYS_IPC_RECV:if(!has_cap(PROCESS_CAP_IPC))return SYSCALL_ERR;{if(arg1==0||arg1>IPC_MESSAGE_MAX||!usercopy_validate_write(arg0,arg1)||(arg2&&!usercopy_validate_write(arg2,sizeof(uint32_t))))return SYSCALL_ERR;void *buffer=kmalloc(arg1);if(!buffer)return SYSCALL_ERR;uint32_t sender=0;int32_t n=ipc_recv(process_current_pid(),buffer,arg1,&sender);if(n>0&&(syscall_copy_out(arg0,buffer,(uint32_t)n)!=0||(arg2&&syscall_copy_out(arg2,&sender,sizeof(sender))!=0)))n=-1;kfree(buffer);if(n==IPC_RECV_EMPTY)return LIONOS_IPC_EMPTY;return n<0?SYSCALL_ERR:(uint32_t)n;}
case LIONOS_SYS_IPC_PENDING:if(!has_cap(PROCESS_CAP_IPC))return SYSCALL_ERR;return ipc_pending(process_current_pid());
case LIONOS_SYS_IPC_RECV_BLOCKING:if(!has_cap(PROCESS_CAP_IPC))return SYSCALL_ERR;{if(arg1==0||arg1>IPC_MESSAGE_MAX||!usercopy_validate_write(arg0,arg1)||(arg2&&!usercopy_validate_write(arg2,sizeof(uint32_t))))return SYSCALL_ERR;void *buffer=kmalloc(arg1);if(!buffer)return SYSCALL_ERR;uint32_t sender=0;int32_t n=ipc_recv_blocking(process_current_pid(),buffer,arg1,&sender);if(n>0&&(syscall_copy_out(arg0,buffer,(uint32_t)n)!=0||(arg2&&syscall_copy_out(arg2,&sender,sizeof(sender))!=0)))n=-1;kfree(buffer);if(n==PROCESS_WAIT_BLOCKED)return PROCESS_WAIT_BLOCKED;return n<0?SYSCALL_ERR:(uint32_t)n;}
case LIONOS_SYS_THREAD_CREATE:if(!has_cap(PROCESS_CAP_PROCESS)||!usercopy_validate_read(arg0,1u))return SYSCALL_ERR;return process_create_user_thread(arg0);
case LIONOS_SYS_THREAD_JOIN:if(!has_cap(PROCESS_CAP_PROCESS)||!usercopy_validate_write(arg1,sizeof(uint32_t)))return SYSCALL_ERR;{int32_t r=process_join_thread(arg0,arg1);return r==PROCESS_WAIT_BLOCKED?(uint32_t)PROCESS_WAIT_BLOCKED:(uint32_t)r;}
case LIONOS_SYS_THREAD_EXIT:if(!has_cap(PROCESS_CAP_PROCESS))return SYSCALL_ERR;process_exit_thread(arg0);return SYSCALL_OK;
case LIONOS_SYS_PIPE_CREATE:if(!has_cap(PROCESS_CAP_IPC)||!usercopy_validate_write(arg0,sizeof(uint32_t))||!usercopy_validate_write(arg1,sizeof(uint32_t))||(arg0<arg1+sizeof(uint32_t)&&arg1<arg0+sizeof(uint32_t)))return SYSCALL_ERR;{uint32_t read_end=0,write_end=0;int32_t r=pipe_create(&read_end,&write_end);if(r<0)return(uint32_t)r;if(syscall_copy_out(arg0,&read_end,sizeof(read_end))!=0||syscall_copy_out(arg1,&write_end,sizeof(write_end))!=0){(void)pipe_close(read_end);(void)pipe_close(write_end);return SYSCALL_ERR;}return(uint32_t)r;}
case LIONOS_SYS_PIPE_READ:if(!has_cap(PROCESS_CAP_IPC)||arg2==0||arg2>IPC_MESSAGE_MAX||!usercopy_validate_write(arg1,arg2))return SYSCALL_ERR;{void *buffer=kmalloc(arg2);if(!buffer)return SYSCALL_ERR;int32_t r=pipe_read(arg0,buffer,arg2);if(r>0&&syscall_copy_out(arg1,buffer,(uint32_t)r)!=0)r=-1;kfree(buffer);return r==PIPE_BLOCKED?(uint32_t)PROCESS_WAIT_BLOCKED:(uint32_t)r;}
case LIONOS_SYS_PIPE_WRITE:if(!has_cap(PROCESS_CAP_IPC)||arg2==0||arg2>IPC_MESSAGE_MAX||!usercopy_validate_read(arg1,arg2))return SYSCALL_ERR;{void *buffer=syscall_copy_in(arg1,arg2);if(!buffer)return SYSCALL_ERR;int32_t r=pipe_write(arg0,buffer,arg2);kfree(buffer);return r==PIPE_BLOCKED?(uint32_t)PROCESS_WAIT_BLOCKED:(uint32_t)r;}
case LIONOS_SYS_PIPE_CLOSE:if(!has_cap(PROCESS_CAP_IPC))return SYSCALL_ERR;return(uint32_t)pipe_close(arg0);
case LIONOS_SYS_GETPPID:{struct process*p=process_current();return p?p->parent_pid:0u;}
case LIONOS_SYS_KILL:if(!has_cap(PROCESS_CAP_PROCESS))return SYSCALL_ERR;if(!process_exists(arg0)||arg0==process_current_pid()||!process_is_descendant_or_child(arg0,process_current_pid())||arg1==0||arg1>LIONOS_SIG_MAX)return SYSCALL_ERR;return(uint32_t)process_signal(arg0,arg1);
case LIONOS_SYS_GETSTATE:return(uint32_t)process_get_state(arg0);
case LIONOS_SYS_SIGPENDING:return process_signal_pending(arg0);
case LIONOS_SYS_NET_SEND:if(!has_cap(PROCESS_CAP_NET))return SYSCALL_ERR;if(arg2==0||arg3==0||arg3>NET_PACKET_MAX||!usercopy_validate_read(arg2,arg3))return SYSCALL_ERR;{void *buffer=syscall_copy_in(arg2,arg3);if(!buffer)return SYSCALL_ERR;int32_t r=net_send(arg0,(uint16_t)(arg1>>16),(uint16_t)arg1,buffer,arg3);kfree(buffer);return(uint32_t)r;}
case LIONOS_SYS_NET_RECV:if(!has_cap(PROCESS_CAP_NET))return SYSCALL_ERR;{if(arg2==0||arg2>NET_PACKET_MAX||!usercopy_validate_write(arg1,arg2)||(arg3&&!usercopy_validate_write(arg3,sizeof(uint32_t)))||(arg4&&!usercopy_validate_write(arg4,sizeof(uint16_t))))return SYSCALL_ERR;void *buffer=kmalloc(arg2);if(!buffer)return SYSCALL_ERR;uint32_t sip=0;uint16_t sport=0;int32_t n=net_recv((uint16_t)arg0,buffer,arg2,&sip,&sport);if(n>0&&(syscall_copy_out(arg1,buffer,(uint32_t)n)!=0||(arg3&&syscall_copy_out(arg3,&sip,sizeof(sip))!=0)||(arg4&&syscall_copy_out(arg4,&sport,sizeof(sport))!=0)))n=-1;kfree(buffer);if(n==NET_RECV_EMPTY)return LIONOS_NET_EMPTY;return n<0?SYSCALL_ERR:(uint32_t)n;}
case LIONOS_SYS_NET_PENDING:if(!has_cap(PROCESS_CAP_NET))return SYSCALL_ERR;return net_pending((uint16_t)arg0);
case LIONOS_SYS_NET_GETIP:if(!has_cap(PROCESS_CAP_NET))return SYSCALL_ERR;return net_local_ip();
case LIONOS_SYS_NET_PING:if(!has_cap(PROCESS_CAP_NET))return SYSCALL_ERR;return (uint32_t)net_ping(arg0);
default:return SYSCALL_ERR;}}
void syscall_init(void){(void)syscall_dispatch;}
uint32_t syscall_handle(uint32_t number,uint32_t arg0,uint32_t arg1,uint32_t arg2,uint32_t arg3,uint32_t arg4){return syscall_dispatch(number,arg0,arg1,arg2,arg3,arg4);}
