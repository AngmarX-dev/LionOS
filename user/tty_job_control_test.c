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

    attributes.flags&=~LIONOS_TTY_ATTR_TOSTOP;
    if(lion_tty_setattr(&attributes)!=0)
        return 21;
    return 0;
}
