#include <stdint.h>
#include "user_api.h"

static int cleanup_child(uint32_t child,int result){
    if(child!=0u&&child!=LIONOS_SYSCALL_ERR){
        if(lion_getstate(child)!=LIONOS_PROCESS_ZOMBIE)
            (void)lion_kill(child,LIONOS_SIG_TERM);
        int32_t ignored=0;
        (void)lion_waitpid(child,&ignored);
    }
    return result;
}

int main(void){
    struct lion_tty_attributes attributes;
    uint8_t byte=0u;
    static const char output[]="x";
    int32_t status=-1;
    if(lion_tty_claim()!=0||lion_tty_getattr(&attributes)!=0)
        return 1;
    attributes.flags|=LIONOS_TTY_ATTR_TOSTOP;
    if(lion_tty_setattr(&attributes)!=0)
        return 2;

    uint32_t child=lion_fork();
    if(child==0u){
        uint32_t pid=lion_getpid();
        if(lion_setpgid(0u,0u)!=0)
            lion_exit_code(10u);
        if(lion_fread(0,&byte,1u)!=-1)
            lion_exit_code(11u);
        if(lion_fwrite(1,output,1u)!=-1)
            lion_exit_code(12u);
        uint32_t pending=lion_sigpending(pid);
        if((pending&(1u<<(LIONOS_SIG_TTIN-1u)))==0u||
           (pending&(1u<<(LIONOS_SIG_TTOU-1u)))==0u)
            lion_exit_code(13u);
        lion_exit_code(0u);
    }
    if(child==LIONOS_SYSCALL_ERR)
        return 3;

    if(lion_waitpid_options(child,&status,LIONOS_WUNTRACED)!=(int32_t)child||
       !LIONOS_WIFSTOPPED(status)||LIONOS_WSTOPSIG(status)!=LIONOS_SIG_TTIN)
        return cleanup_child(child,4);
    status=-1;
    if(lion_waitpid_options(child,&status,LIONOS_WNOHANG|LIONOS_WCONTINUED)!=0)
        return cleanup_child(child,5);
    if(lion_kill(child,LIONOS_SIG_CONT)!=0)
        return cleanup_child(child,6);
    status=-1;
    if(lion_waitpid_options(child,&status,LIONOS_WCONTINUED)!=(int32_t)child||
       !LIONOS_WIFCONTINUED(status))
        return cleanup_child(child,7);
    if(lion_waitpid_options(child,&status,LIONOS_WNOHANG|LIONOS_WCONTINUED)!=0)
        return cleanup_child(child,8);

    status=-1;
    if(lion_waitpid_options(child,&status,LIONOS_WUNTRACED)!=(int32_t)child||
       !LIONOS_WIFSTOPPED(status)||LIONOS_WSTOPSIG(status)!=LIONOS_SIG_TTOU)
        return cleanup_child(child,9);
    if(lion_kill(child,LIONOS_SIG_CONT)!=0)
        return cleanup_child(child,10);
    status=-1;
    if(lion_waitpid_options(child,&status,LIONOS_WCONTINUED)!=(int32_t)child||
       !LIONOS_WIFCONTINUED(status))
        return cleanup_child(child,11);
    status=-1;
    if(lion_waitpid(child,&status)!=(int32_t)child||status!=0)
        return cleanup_child(child,12);

    uint32_t signaled_child=lion_fork();
    if(signaled_child==0u){
        for(;;)(void)lion_yield();
    }
    if(signaled_child==LIONOS_SYSCALL_ERR)
        return 14;
    if(lion_kill(signaled_child,LIONOS_SIG_TTIN)!=0)
        return cleanup_child(signaled_child,15);
    status=-1;
    if(lion_waitpid_options(signaled_child,&status,LIONOS_WUNTRACED)!=(int32_t)signaled_child||
       !LIONOS_WIFSTOPPED(status)||LIONOS_WSTOPSIG(status)!=LIONOS_SIG_TTIN)
        return cleanup_child(signaled_child,16);
    if(lion_kill(signaled_child,LIONOS_SIG_CONT)!=0)
        return cleanup_child(signaled_child,17);
    status=-1;
    if(lion_waitpid_options(signaled_child,&status,LIONOS_WCONTINUED)!=(int32_t)signaled_child||
       !LIONOS_WIFCONTINUED(status))
        return cleanup_child(signaled_child,18);
    if(lion_kill(signaled_child,LIONOS_SIG_TERM)!=0)
        return cleanup_child(signaled_child,19);
    status=-1;
    if(lion_waitpid(signaled_child,&status)!=(int32_t)signaled_child||
       status!=(int32_t)(128u+LIONOS_SIG_TERM))
        return cleanup_child(signaled_child,20);

    uint32_t sync_read=0u,sync_write=0u,report_read=0u,report_write=0u;
    if(lion_pipe_create(&sync_read,&sync_write)!=0)
        return 22;
    if(lion_pipe_create(&report_read,&report_write)!=0){
        (void)lion_pipe_close(sync_read);(void)lion_pipe_close(sync_write);
        return 22;
    }
    uint32_t orphan_parent=lion_fork();
    if(orphan_parent==0u){
        uint32_t orphan_child=lion_fork();
        if(orphan_child==0u){
            uint32_t pid=lion_getpid();
            uint32_t ready=(lion_setpgid(0u,0u)==0)?0xA11CE001u:0u;
            if(lion_pipe_write(sync_write,&ready,sizeof(ready))!=(int32_t)sizeof(ready))
                lion_exit_code(30u);
            if(ready!=0xA11CE001u)lion_exit_code(36u);
            while(lion_getppid()!=1u)(void)lion_yield();
            int32_t tty_result=lion_tty_read(&byte,1u);
            uint32_t pending=lion_sigpending(pid);
            uint32_t result=(lion_getppid()==1u&&tty_result==-1&&
                (pending&(1u<<(LIONOS_SIG_HUP-1u)))!=0u&&
                (pending&(1u<<(LIONOS_SIG_TTIN-1u)))==0u&&
                lion_getstate(pid)!=LIONOS_PROCESS_STOPPED)?0u:31u;
            (void)lion_pipe_write(report_write,&result,sizeof(result));
            lion_exit_code(result);
        }
        if(orphan_child==LIONOS_SYSCALL_ERR){
            uint32_t fail=0u;(void)lion_pipe_write(report_write,&fail,sizeof(fail));
            lion_exit_code(32u);
        }
        uint32_t ready=0u;
        if(lion_pipe_read(sync_read,&ready,sizeof(ready))!=(int32_t)sizeof(ready)||
           ready!=0xA11CE001u){
            uint32_t fail=0u;(void)lion_pipe_write(report_write,&fail,sizeof(fail));
            lion_exit_code(33u);
        }
        if(lion_pipe_write(report_write,&orphan_child,sizeof(orphan_child))!=(int32_t)sizeof(orphan_child)||
           lion_kill(orphan_child,LIONOS_SIG_STOP)!=0)
            lion_exit_code(34u);
        int32_t orphan_status=-1;
        if(lion_waitpid_options(orphan_child,&orphan_status,LIONOS_WUNTRACED)!=(int32_t)orphan_child||
           !LIONOS_WIFSTOPPED(orphan_status))
            lion_exit_code(35u);
        lion_exit_code(0u);
    }
    if(orphan_parent==LIONOS_SYSCALL_ERR){
        (void)lion_pipe_close(sync_read);(void)lion_pipe_close(report_read);(void)lion_pipe_close(sync_write);(void)lion_pipe_close(report_write);
        return 23;
    }
    uint32_t orphan_pid=0u;
    if(lion_pipe_read(report_read,&orphan_pid,sizeof(orphan_pid))!=(int32_t)sizeof(orphan_pid)||
       orphan_pid==0u){
        return cleanup_child(orphan_parent,24);
    }
    status=-1;
    if(lion_waitpid(orphan_parent,&status)!=(int32_t)orphan_parent||status!=0)
        return cleanup_child(orphan_parent,25);
    uint32_t orphan_result=0xFFFFFFFFu;
    if(lion_pipe_read(report_read,&orphan_result,sizeof(orphan_result))!=(int32_t)sizeof(orphan_result)||
       orphan_result!=0u){
        (void)lion_pipe_close(sync_read);(void)lion_pipe_close(report_read);(void)lion_pipe_close(sync_write);(void)lion_pipe_close(report_write);
        return 26;
    }
    (void)lion_pipe_close(sync_read);(void)lion_pipe_close(report_read);
    (void)lion_pipe_close(sync_write);(void)lion_pipe_close(report_write);

    attributes.flags&=~LIONOS_TTY_ATTR_TOSTOP;
    if(lion_tty_setattr(&attributes)!=0)
        return 27;
    return 0;
}
