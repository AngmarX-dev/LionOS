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

static int32_t waitpid_bounded(uint32_t pid,int32_t *status){
    for(uint32_t attempt=0u;attempt<20000u;++attempt){
        int32_t result=lion_waitpid_options(pid,status,LIONOS_WNOHANG);
        if(result==(int32_t)pid||result<0)return result;
        (void)lion_yield();
    }
    return -2;
}

static int test_setsid_orphan_transition(void){
    int32_t status=-1;
    uint32_t parent=lion_fork();
    if(parent==0u){
        uint32_t child=lion_fork();
        if(child==0u){
            for(;;)(void)lion_yield();
        }
        if(child==LIONOS_SYSCALL_ERR)lion_exit_code(43u);
        if(lion_setpgid(child,child)!=0||lion_kill(child,LIONOS_SIG_STOP)!=0)
            lion_exit_code(41u);
        if(lion_waitpid_options(child,&status,LIONOS_WUNTRACED)!=(int32_t)child||
           !LIONOS_WIFSTOPPED(status))lion_exit_code(44u);
        if(lion_setsid()<0)lion_exit_code(45u);
        status=-1;
        if(waitpid_bounded(child,&status)!=(int32_t)child)
            lion_exit_code(147u);
        if(status!=(int32_t)(128u+LIONOS_SIG_HUP))
            lion_exit_code(46u);
        lion_exit_code(0u);
    }
    if(parent==LIONOS_SYSCALL_ERR)return 48;
    int32_t waited=waitpid_bounded(parent,&status);
    if(waited!=(int32_t)parent)return 50;
    if(status!=0)return 51+(int)(status&0x3Fu);
    return 0;
}

struct orphan_transition_pids {
    uint32_t group_parent;
    uint32_t child;
};

static int test_setpgid_orphan_transition(void){
    uint32_t sync_read=0u,sync_write=0u;
    int32_t status=-1;
    struct orphan_transition_pids pids={0u,0u};
    if(lion_pipe_create(&sync_read,&sync_write)!=0)return 70;

    uint32_t session_parent=lion_fork();
    if(session_parent==0u){
        (void)lion_pipe_close(sync_read);
        uint32_t group_parent=lion_fork();
        if(group_parent==0u){
            uint32_t child=lion_fork();
            if(child==0u){
                /* Do not keep the synchronization pipe alive on failure. */
                (void)lion_pipe_close(sync_write);
                for(;;)(void)lion_yield();
            }
            if(child==LIONOS_SYSCALL_ERR)lion_exit_code(71u);
            if(lion_setpgid(child,child)!=0||
               lion_kill(child,LIONOS_SIG_STOP)!=0)
                lion_exit_code(72u);
            if(lion_waitpid_options(child,&status,LIONOS_WUNTRACED)!=(int32_t)child||
               !LIONOS_WIFSTOPPED(status))
                lion_exit_code(73u);

            /* Put both processes in the child's group while the session
               parent is still in this session: the group is not orphaned yet. */
            if(lion_setpgid(0u,child)!=0)lion_exit_code(74u);
            pids.group_parent=lion_getpid();
            pids.child=child;
            if(lion_pipe_write(sync_write,&pids,sizeof(pids))!=(int32_t)sizeof(pids))
                lion_exit_code(75u);
            (void)lion_pipe_close(sync_write);
            for(;;)(void)lion_yield();
        }
        if(group_parent==LIONOS_SYSCALL_ERR)lion_exit_code(76u);
        (void)lion_pipe_close(sync_write);
        if(lion_pipe_read(sync_read,&pids,sizeof(pids))!=(int32_t)sizeof(pids)||
           pids.group_parent!=group_parent||pids.child==0u)
            lion_exit_code(77u);
        (void)lion_pipe_close(sync_read);

        /* Leaving the session now orphans the already-existing group. */
        if(lion_setsid()<0)lion_exit_code(78u);
        status=-1;
        if(waitpid_bounded(group_parent,&status)!=(int32_t)group_parent)
            lion_exit_code(180u);
        if(status!=(int32_t)(128u+LIONOS_SIG_HUP))
            lion_exit_code(79u);

        /* The stopped child was reparented to init when its parent exited. */
        uint32_t child_exited=0u;
        for(uint32_t attempt=0u;attempt<20000u;++attempt){
            /* PID 1 adopts and auto-reaps this child after group_parent exits. */
            if(lion_getstate(pids.child)<0){
                child_exited=1u;
                break;
            }
            (void)lion_yield();
        }
        if(!child_exited)lion_exit_code(181u);
        lion_exit_code(0u);
    }

    if(session_parent==LIONOS_SYSCALL_ERR){
        (void)lion_pipe_close(sync_read);
        (void)lion_pipe_close(sync_write);
        return 80;
    }
    (void)lion_pipe_close(sync_read);
    (void)lion_pipe_close(sync_write);
    status=-1;
    if(waitpid_bounded(session_parent,&status)!=(int32_t)session_parent)
        return 183;
    return status==0?0:184;
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
        if(lion_setpgid(0u,0u)!=0)
            lion_exit_code(10u);
        if(lion_fread(0,&byte,1u)!=-1)
            lion_exit_code(11u);
        if(lion_fwrite(1,output,1u)!=-1)
            lion_exit_code(12u);
        lion_exit_code(0u);
    }
    if(child==LIONOS_SYSCALL_ERR)
        return 3;

    if(lion_waitpid_options(child,&status,LIONOS_WUNTRACED)!=(int32_t)child||
       !LIONOS_WIFSTOPPED(status)||LIONOS_WSTOPSIG(status)!=LIONOS_SIG_TTIN)
        return cleanup_child(child,4);
    if((lion_sigpending(child)&(1u<<(LIONOS_SIG_TTIN-1u)))==0u)
        return cleanup_child(child,13);
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
    if((lion_sigpending(child)&(1u<<(LIONOS_SIG_TTOU-1u)))==0u)
        return cleanup_child(child,14);
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
            uint32_t ready=(lion_setpgid(0u,0u)==0)?0xA11CE001u:0u;
            if(lion_pipe_write(sync_write,&ready,sizeof(ready))!=(int32_t)sizeof(ready))
                lion_exit_code(30u);
            if(ready!=0xA11CE001u)lion_exit_code(36u);
            while(lion_getppid()!=1u)(void)lion_yield();
            for(;;)(void)lion_yield();
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
    (void)lion_pipe_close(sync_read);(void)lion_pipe_close(report_read);
    (void)lion_pipe_close(sync_write);(void)lion_pipe_close(report_write);
    uint32_t orphan_exited=0u;
    for(uint32_t attempt=0u;attempt<10000u;++attempt){
        if(lion_getstate(orphan_pid)==LIONOS_PROCESS_ZOMBIE){
            orphan_exited=1u;
            break;
        }
        (void)lion_yield();
    }
    if(!orphan_exited)
        return 26;

    int transition_result=test_setsid_orphan_transition();
    if(transition_result)return 63+transition_result;
    transition_result=test_setpgid_orphan_transition();
    if(transition_result)return 126+transition_result;

    attributes.flags&=~LIONOS_TTY_ATTR_TOSTOP;
    if(lion_tty_setattr(&attributes)!=0)
        return 27;
    return 0;
}
