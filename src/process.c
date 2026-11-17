#include <stdint.h>
#include "memory.h"
#include "paging.h"
#include "process.h"
#include "uapi.h"
#include "signal.h"
#include "tss.h"
#include "spinlock.h"
#include "cpu.h"
#include "debug.h"
#include "pipe.h"
#include "vfs.h"
#include "tty.h"
extern void lionos_kernel_idle(void);

#define PROCESS_WAIT_ANY 0u
#define PROCESS_WAIT_BLOCKED (-2)
#define PROCESS_SYSCALL_ERR 0xFFFFFFFFu
#define PAGE_SIZE 4096u
#define USER_CODE_SEL 0x2Bu
#define USER_DATA_SEL 0x33u

static struct process processes[LIONOS_PROCESS_MAX];
static struct process *current_by_cpu[LIONOS_MAX_CPUS];
struct scheduler_cpu_state{uint32_t cursor;uint32_t schedule_count;uint32_t steal_count;uint32_t timer_ticks;};
static struct scheduler_cpu_state scheduler_cpu[LIONOS_MAX_CPUS];
static uint32_t next_pid;
static uint32_t next_tid;
static struct spinlock process_lock;
static volatile uint32_t reschedule_requested[LIONOS_MAX_CPUS];
static uint32_t fpu_enabled;

static void reap_process(struct process *p);
static struct process *find_pid(uint32_t pid);
static uint8_t fpu_initial_state[512] __attribute__((aligned(16)));

static uint32_t quantum_for_priority(uint32_t priority){
    static const uint32_t q[PROCESS_PRIORITY_LEVELS]={2u,4u,8u,12u};
    return q[priority<PROCESS_PRIORITY_LEVELS?priority:PROCESS_PRIORITY_LEVELS-1u];
}
static void fpu_save(struct process *p){if(!fpu_enabled||!p)return;__asm__ volatile("fxsave %0":"=m"(p->fpu_state)::"memory");p->fpu_valid=1u;}
static void fpu_restore(struct process *p){if(!fpu_enabled||!p)return;if(p->fpu_valid)__asm__ volatile("fxrstor %0"::"m"(p->fpu_state):"memory");else __asm__ volatile("fxrstor %0"::"m"(fpu_initial_state):"memory");}

static struct process *current_local(void) { uint32_t cpu = cpu_current_index(); return cpu < LIONOS_MAX_CPUS ? current_by_cpu[cpu] : 0; }
static void set_current_local(struct process *p) { uint32_t cpu = cpu_current_index(); if (cpu < LIONOS_MAX_CPUS) current_by_cpu[cpu] = p; }
static struct process *find_free_slot(void) { for (uint32_t i=0;i<LIONOS_PROCESS_MAX;++i) if (processes[i].state==PROCESS_UNUSED) return &processes[i]; return 0; }
static uint32_t process_index(const struct process *p) { return (uint32_t)(p-processes); }
static int write_user_u32(const struct process *p,uint32_t ptr,uint32_t value) { uint32_t physical,flags; if(!p||ptr<0x10000000u||ptr>0xBFFFFFFCu||(ptr&3u))return -1;uint32_t page=ptr&~(PAGE_SIZE-1u);if(paging_get_user_page(p->page_directory,page,&physical,&flags)!=0)return -1;if(!(flags&0x2u)){if(!(flags&0x200u)||paging_resolve_cow(p->page_directory,page)!=0)return -1;if(paging_get_user_page(p->page_directory,page,&physical,&flags)!=0||!(flags&0x2u))return -1;}*(uint32_t *)(uintptr_t)(physical+(ptr&(PAGE_SIZE-1u)))=value;return 0;}
static void clear_process(struct process *p) { p->pid=0;p->state=PROCESS_UNUSED;p->parent_pid=0;p->pgid=0;p->sid=0;p->exit_code=0;p->wait_pid=0;p->wait_status_ptr=0;p->wait_channel=0;p->wait_kind=0;p->wait_timeout_active=0;p->wait_timeout_ticks=0;p->wait_deadline=0;p->wait_cpu=0;p->wait_timeout_result=0; p->reap_pending=0;p->deferred_kernel_stack=0;p->page_directory=0;p->entry=0;p->user_stack=0; p->kernel_stack_top=0;p->saved_frame=0;p->user_code_page=0;p->user_stack_page=0;p->user_page_count=0;p->pending_signals=0;p->cpu_owner=0;p->priority=PROCESS_PRIORITY_DEFAULT;p->base_priority=PROCESS_PRIORITY_DEFAULT;p->time_slice=0;p->ticks_used=0;p->fpu_valid=0;p->tid=0;p->thread_group_pid=0;p->is_thread=0;p->owns_address_space=0;p->thread_stack_page=0;p->thread_stack_va=0;p->join_tid=0;p->join_status_ptr=0;p->pipe_read_mask=0;p->pipe_write_mask=0;p->fd_closing=0; for(uint32_t i=0;i<PROCESS_FD_MAX;++i){p->fd_used[i]=0;p->fd_object[i]=0;p->fd_backend[i]=0;p->fd_flags[i]=0;p->fd_offset[i]=0;p->fd_path[i][0]=0;} for(uint32_t i=0;i<LIONOS_PROCESS_MAX_USER_PAGES;++i){p->user_pages[i]=0;p->user_page_vas[i]=0;} for(uint32_t i=0;i<PROCESS_FD_MAX;++i){p->fd_used[i]=0;p->fd_backend[i]=0;p->fd_flags[i]=0;p->fd_offset[i]=0;p->fd_path[i][0]=0;} }
static void init_interrupt_frame(struct process *p) { uint32_t *f=(uint32_t *)(uintptr_t)(p->kernel_stack_top-PROCESS_CONTEXT_WORDS*4u); for(uint32_t i=0;i<PROCESS_CONTEXT_WORDS;++i) f[i]=0; f[0]=USER_DATA_SEL;f[1]=USER_DATA_SEL;f[2]=USER_DATA_SEL;f[3]=USER_DATA_SEL; f[14]=p->entry;f[15]=USER_CODE_SEL;f[16]=0x202u;f[17]=p->user_stack;f[18]=USER_DATA_SEL; p->saved_frame=(uint32_t)(uintptr_t)f; }
static void init_bootstrap_frame(void) { struct process *b=&processes[0]; void *s=page_alloc(); if(!s){b->kernel_stack_top=0;b->saved_frame=0;return;} b->kernel_stack_top=(uint32_t)(uintptr_t)s+4096u;b->page_directory=paging_kernel_directory(); b->entry=(uint32_t)(uintptr_t)&process_schedule;b->user_stack=b->kernel_stack_top; uint32_t *f=(uint32_t *)(uintptr_t)(b->kernel_stack_top-PROCESS_CONTEXT_WORDS*4u); for(uint32_t i=0;i<PROCESS_CONTEXT_WORDS;++i) f[i]=0; f[0]=0x10u;f[1]=0x10u;f[2]=0x10u;f[3]=0x10u;f[14]=(uint32_t)(uintptr_t)&lionos_kernel_idle; f[15]=0x08u;f[16]=0x202u;f[17]=b->kernel_stack_top;f[18]=0x10u; b->saved_frame=(uint32_t)(uintptr_t)f; }
void process_init(void) { fpu_enabled=(cpu_fpu_sse_init()==0u)?1u:0u; if(fpu_enabled){__asm__ volatile("fninit; fxsave %0":"=m"(fpu_initial_state)::"memory");} spinlock_init(&process_lock); uint32_t flags=spinlock_irqsave_acquire(&process_lock); for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i) clear_process(&processes[i]); for(uint32_t i=0;i<LIONOS_MAX_CPUS;++i){current_by_cpu[i]=0;scheduler_cpu[i].cursor=0;scheduler_cpu[i].schedule_count=0;scheduler_cpu[i].steal_count=0;scheduler_cpu[i].timer_ticks=0;reschedule_requested[i]=0u;} next_pid=2;next_tid=1;processes[0].pid=1;processes[0].pgid=1;processes[0].sid=1;processes[0].thread_group_pid=1;processes[0].state=PROCESS_RUNNING;init_bootstrap_frame(); set_current_local(&processes[0]); spinlock_irqrestore_release(&process_lock,flags); }
struct process *process_current(void){return current_local();}
uint32_t process_current_tid(void){struct process*p=current_local();return p?p->tid:0u;}
struct process *process_at(uint32_t i){return i<LIONOS_PROCESS_MAX?&processes[i]:0;}
uint32_t process_current_pid(void){struct process*p=current_local();return p?p->pid:0;}

int32_t process_get_pgid(uint32_t pid){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *target=0;
    if(pid==0u){
        struct process *current=current_local();
        if(current&&current->is_thread)
            target=find_pid(current->thread_group_pid);
        else
            target=current;
    }else{
        target=find_pid(pid);
    }
    int32_t result=target&&target->state!=PROCESS_ZOMBIE?(int32_t)target->pgid:-1;
    spinlock_irqrestore_release(&process_lock,flags);
    return result;
}

int32_t process_tty_set_foreground(uint32_t sid,uint32_t pgid){
    if(!sid||!pgid)return -1;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    int found=0;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        const struct process *member=&processes[i];
        if(member->state!=PROCESS_UNUSED&&member->state!=PROCESS_ZOMBIE&&
           !member->is_thread&&member->sid==sid&&member->pgid==pgid){
            found=1;
            break;
        }
    }
    int32_t result=found?tty_set_foreground_pgid(sid,pgid):-1;
    spinlock_irqrestore_release(&process_lock,flags);
    if(result==0)tty_wake_readers();
    return result;
}

static void process_release_tty_if_session_empty(uint32_t sid){
    if(!sid)return;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    int live=0;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        const struct process *member=&processes[i];
        if(member->state!=PROCESS_UNUSED&&member->state!=PROCESS_ZOMBIE&&
           !member->is_thread&&member->sid==sid){
            live=1;
            break;
        }
    }
    spinlock_irqrestore_release(&process_lock,flags);
    if(!live)(void)tty_release_session(sid);
}

int32_t process_get_sid(uint32_t pid){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *target=0;
    if(pid==0u){
        struct process *current=current_local();
        if(current&&current->is_thread)
            target=find_pid(current->thread_group_pid);
        else
            target=current;
    }else{
        target=find_pid(pid);
    }
    int32_t result=target&&target->state!=PROCESS_ZOMBIE?(int32_t)target->sid:-1;
    spinlock_irqrestore_release(&process_lock,flags);
    return result;
}

int32_t process_create_session(void){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *current=current_local();
    if(!current||current==&processes[0]||current->is_thread||
       current->pid==current->pgid){
        spinlock_irqrestore_release(&process_lock,flags);
        return -1;
    }
    uint32_t group_pid=current->pid;
    current->sid=group_pid;
    current->pgid=group_pid;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *member=&processes[i];
        if(member->state!=PROCESS_UNUSED&&member->is_thread&&
           member->thread_group_pid==group_pid){
            member->sid=group_pid;
            member->pgid=group_pid;
        }
    }
    spinlock_irqrestore_release(&process_lock,flags);
    return (int32_t)group_pid;
}

int32_t process_set_pgid(uint32_t pid,uint32_t pgid){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *current=current_local();
    if(!current||current==&processes[0]||current->is_thread){
        spinlock_irqrestore_release(&process_lock,flags);return -1;
    }
    uint32_t target_pid=pid?pid:current->pid;
    struct process *target=find_pid(target_pid);
    if(!target||target->is_thread||target->state==PROCESS_ZOMBIE||
       target->sid!=current->sid||target->sid==target->pid||
       (target!=current&&target->parent_pid!=current->pid)){
        spinlock_irqrestore_release(&process_lock,flags);return -1;
    }
    uint32_t new_pgid=pgid?pgid:target->pid;
    if(new_pgid!=target->pid&&new_pgid!=current->pgid){
        struct process *leader=find_pid(new_pgid);
        if(!leader||leader->is_thread||leader->state==PROCESS_ZOMBIE||
           leader->sid!=target->sid||leader->pid!=leader->pgid||
           leader->parent_pid!=current->pid){
            spinlock_irqrestore_release(&process_lock,flags);return -1;
        }
    }
    uint32_t group=target->thread_group_pid?target->thread_group_pid:target->pid;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *member=&processes[i];
        if(member->state!=PROCESS_UNUSED&&member->thread_group_pid==group)
            member->pgid=new_pgid;
    }
    spinlock_irqrestore_release(&process_lock,flags);
    return 0;
}
struct process *process_pipe_owner(void){
    struct process *current=current_local();
    if(!current)return 0;
    if(!current->is_thread)return current->pid?current:0;
    uint32_t group=current->thread_group_pid;
    if(!group)return 0;
    for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i)
        if(processes[i].state!=PROCESS_UNUSED&&!processes[i].is_thread&&
           processes[i].pid==group)return &processes[i];
    return 0;
}
struct process *process_fd_owner(void){return process_pipe_owner();}
const char *process_state_name(uint32_t s){switch(s){case PROCESS_READY:return "READY";case PROCESS_RUNNING:return "RUNNING";case PROCESS_ZOMBIE:return "ZOMBIE";case PROCESS_WAITING:return "WAITING";case PROCESS_STOPPED:return "STOPPED";default:return "UNUSED";}}

static struct process *process_create_ex_vas_locked(uint32_t entry,uint32_t stack,uint32_t pd,const uint32_t *pages,const uint32_t *vas,uint32_t count){ struct process*p=find_free_slot(); if(!p||!pd||!pages||!vas||!count||count>LIONOS_PROCESS_MAX_USER_PAGES)return 0; void *ks=page_alloc();if(!ks)return 0; p->pid=next_pid++;if(next_pid==0)next_pid=2;p->tid=next_tid++;if(next_tid==0)next_tid=1;p->thread_group_pid=p->pid;p->is_thread=0;p->owns_address_space=1;p->thread_stack_page=0;p->thread_stack_va=0;p->join_tid=0;p->join_status_ptr=0;p->state=PROCESS_READY; struct process *parent=current_local();p->parent_pid=parent?parent->pid:1;p->pgid=p->pid;p->sid=p->pid; p->exit_code=0;p->wait_pid=0;p->wait_status_ptr=0;p->wait_channel=0;p->wait_kind=0;p->reap_pending=0;p->deferred_kernel_stack=0; p->page_directory=pd;p->entry=entry;p->user_stack=stack;p->kernel_stack_top=(uint32_t)(uintptr_t)ks+4096u; p->saved_frame=0;p->user_page_count=count;p->user_code_page=pages[0];p->user_stack_page=pages[count-1u];p->pending_signals=0;p->capabilities=PROCESS_CAP_USER_DEFAULT;p->cpu_owner=cpu_current_index();p->priority=PROCESS_PRIORITY_DEFAULT;p->base_priority=PROCESS_PRIORITY_DEFAULT;p->time_slice=quantum_for_priority(PROCESS_PRIORITY_DEFAULT);p->ticks_used=0;p->fpu_valid=0;p->pipe_read_mask=0;p->pipe_write_mask=0; for(uint32_t i=0;i<count;++i){p->user_pages[i]=pages[i];p->user_page_vas[i]=vas[i];} for(uint32_t i=count;i<LIONOS_PROCESS_MAX_USER_PAGES;++i){p->user_pages[i]=0;p->user_page_vas[i]=0;} init_interrupt_frame(p);return p;}
struct process *process_create_ex_vas(uint32_t entry,uint32_t stack,uint32_t pd,const uint32_t *pages,const uint32_t *vas,uint32_t count){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *p=process_create_ex_vas_locked(entry,stack,pd,pages,vas,count);
    if(p)p->state=PROCESS_STOPPED;
    spinlock_irqrestore_release(&process_lock,flags);
    if(!p)return 0;
    if(vfs_stdio_init_process(p)!=0){
        flags=spinlock_irqsave_acquire(&process_lock);
        if(p->state==PROCESS_STOPPED){
            if(p->kernel_stack_top)
                page_free((void *)(uintptr_t)(p->kernel_stack_top-4096u));
            clear_process(p);
        }
        spinlock_irqrestore_release(&process_lock,flags);
        /* The caller still owns the user pages and address space on failure. */
        return 0;
    }
    flags=spinlock_irqsave_acquire(&process_lock);
    if(p->state==PROCESS_STOPPED)p->state=PROCESS_READY;
    spinlock_irqrestore_release(&process_lock,flags);
    return p;
}
struct process *process_create_ex(uint32_t entry,uint32_t stack,uint32_t pd,const uint32_t *pages,uint32_t count){ uint32_t vas[LIONOS_PROCESS_MAX_USER_PAGES];for(uint32_t i=0;i<count&&i<LIONOS_PROCESS_MAX_USER_PAGES;++i)vas[i]=i*4096u;return process_create_ex_vas(entry,stack,pd,pages,vas,count); }
struct process *process_create(uint32_t entry,uint32_t stack,uint32_t pd,uint32_t code,uint32_t user_stack_page){ uint32_t p[2]={code,user_stack_page},v[2]={0x10000000u,0x10001000u};return process_create_ex_vas(entry,stack,pd,p,v,2u); }

int process_exec_replace_current(uint32_t entry,uint32_t stack,uint32_t pd,const uint32_t *pages,const uint32_t *vas,uint32_t count){ uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*c=current_local(); if(!c||c==&processes[0]||c->is_thread||!pd||!pages||!vas||!count||count>LIONOS_PROCESS_MAX_USER_PAGES){spinlock_irqrestore_release(&process_lock,flags);return -1;} void*new_ks=page_alloc();if(!new_ks){spinlock_irqrestore_release(&process_lock,flags);return -1;} for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){struct process*t=&processes[i];if(t!=c&&t->state!=PROCESS_UNUSED&&t->is_thread&&t->thread_group_pid==c->thread_group_pid){t->state=PROCESS_ZOMBIE;reap_process(t);}} uint32_t old_pd=c->page_directory,old_ks=c->kernel_stack_top,old_pages[LIONOS_PROCESS_MAX_USER_PAGES],old_count=c->user_page_count; for(uint32_t i=0;i<old_count;++i)old_pages[i]=c->user_pages[i]; c->page_directory=pd;c->entry=entry;c->user_stack=stack;c->kernel_stack_top=(uint32_t)(uintptr_t)new_ks+4096u;c->saved_frame=0; c->user_page_count=count;c->user_code_page=pages[0];c->user_stack_page=pages[count-1u];c->exit_code=0;c->wait_pid=0;c->wait_status_ptr=0;c->reap_pending=0;c->pending_signals=0;c->deferred_kernel_stack=old_ks; for(uint32_t i=0;i<count;++i){c->user_pages[i]=pages[i];c->user_page_vas[i]=vas[i];} for(uint32_t i=count;i<LIONOS_PROCESS_MAX_USER_PAGES;++i){c->user_pages[i]=0;c->user_page_vas[i]=0;} paging_switch_address_space(pd);tss_set_kernel_stack(c->kernel_stack_top);init_interrupt_frame(c); for(uint32_t i=0;i<old_count;++i)if(old_pages[i])page_free((void *)(uintptr_t)old_pages[i]); if(old_pd&&old_pd!=pd)paging_destroy_address_space(old_pd); spinlock_irqrestore_release(&process_lock,flags);return 0; }

uint32_t process_fork_current(uint32_t *parent_frame){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process*parent=current_local();
    struct process*parent_owner=process_pipe_owner();
    if(!parent||parent==&processes[0]||!parent_owner||!parent_frame||!parent->user_page_count){spinlock_irqrestore_release(&process_lock,flags);return PROCESS_SYSCALL_ERR;}
    struct process*slot=find_free_slot();
    if(!slot){spinlock_irqrestore_release(&process_lock,flags);return PROCESS_SYSCALL_ERR;}
    uint32_t pd=paging_create_address_space();
    if(!pd){spinlock_irqrestore_release(&process_lock,flags);return PROCESS_SYSCALL_ERR;}

    uint32_t pages[LIONOS_PROCESS_MAX_USER_PAGES]={0};
    uint32_t vas[LIONOS_PROCESS_MAX_USER_PAGES]={0};
    uint32_t old_flags[LIONOS_PROCESS_MAX_USER_PAGES]={0};
    uint32_t count=parent->user_page_count,retained=0,cowed=0;

    for(uint32_t i=0;i<count;++i){
        if(paging_get_user_page(parent->page_directory,parent->user_page_vas[i],&pages[i],&old_flags[i])!=0)goto fail;
        vas[i]=parent->user_page_vas[i];
        uint32_t child_flags=old_flags[i];
        if(old_flags[i]&0x2u)child_flags=(old_flags[i]&~0x2u)|0x200u;
        if(paging_map_user_page_in(pd,vas[i],pages[i],child_flags)!=0)goto fail;
        page_retain((void*)(uintptr_t)pages[i]);++retained;
    }
    for(uint32_t i=0;i<count;++i){
        if(!(old_flags[i]&0x2u))continue;
        if(paging_set_user_page_flags(parent->page_directory,vas[i],(old_flags[i]&~0x2u)|0x200u)!=0)goto restore;
        ++cowed;
    }

    /* fork() duplicates the caller's architectural FPU/SSE state. FXSAVE is
       non-destructive, so the parent continues with identical live registers. */
    fpu_save(parent);
    slot=process_create_ex_vas_locked(parent->entry,parent->user_stack,pd,pages,vas,count);
    if(!slot)goto restore;
    if(fpu_enabled&&parent->fpu_valid){
        for(uint32_t i=0u;i<sizeof(parent->fpu_state);++i)
            slot->fpu_state[i]=parent->fpu_state[i];
        slot->fpu_valid=1u;
    }
    slot->parent_pid=parent_owner->pid;slot->pgid=parent_owner->pgid;slot->sid=parent->sid;
    slot->tid=next_tid++;if(next_tid==0)next_tid=1;slot->thread_group_pid=slot->pid;slot->is_thread=0;slot->owns_address_space=1;
    slot->cpu_owner=cpu_current_index();
    slot->capabilities=parent->capabilities;
    slot->priority=parent->priority;
    slot->base_priority=parent->base_priority;
    slot->time_slice=quantum_for_priority(slot->priority);
    slot->ticks_used=0;

    /* File descriptions are shared by fork; pipe ownership is cloned separately. */
    {
        uint32_t*cf=(uint32_t*)(uintptr_t)slot->saved_frame;
        for(uint32_t i=0;i<PROCESS_CONTEXT_WORDS;++i)cf[i]=parent_frame[i];
        cf[11]=0;
    }
    {
        uint32_t result=slot->pid;
        slot->state=PROCESS_STOPPED;
        spinlock_irqrestore_release(&process_lock,flags);
        uint32_t cleanup_flags;
        if(vfs_fork_process(parent_owner,slot)!=0){
            cleanup_flags=spinlock_irqsave_acquire(&process_lock);
            if(slot->state==PROCESS_STOPPED)slot->state=PROCESS_ZOMBIE;
            spinlock_irqrestore_release(&process_lock,cleanup_flags);
            (void)process_reap_pid(result);
            return PROCESS_SYSCALL_ERR;
        }
        if(pipe_fork(parent_owner,slot)!=0){
            vfs_close_all_process(slot);
            slot->pipe_read_mask=0u;
            slot->pipe_write_mask=0u;
            cleanup_flags=spinlock_irqsave_acquire(&process_lock);
            if(slot->state==PROCESS_STOPPED)slot->state=PROCESS_ZOMBIE;
            spinlock_irqrestore_release(&process_lock,cleanup_flags);
            (void)process_reap_pid(result);
            return PROCESS_SYSCALL_ERR;
        }
        cleanup_flags=spinlock_irqsave_acquire(&process_lock);
        if(slot->state==PROCESS_STOPPED)slot->state=PROCESS_READY;
        spinlock_irqrestore_release(&process_lock,cleanup_flags);
        return result;
    }
restore:
    for(uint32_t i=0;i<cowed;++i)paging_set_user_page_flags(parent->page_directory,vas[i],old_flags[i]);
fail:
    for(uint32_t i=0;i<retained;++i)page_free((void*)(uintptr_t)pages[i]);
    paging_destroy_address_space(pd);
    spinlock_irqrestore_release(&process_lock,flags);
    return PROCESS_SYSCALL_ERR;
}

static int is_child_of(const struct process*c,uint32_t parent_pid,uint32_t pid){return c->state!=PROCESS_UNUSED&&c->parent_pid==parent_pid&&(pid==PROCESS_WAIT_ANY||c->pid==pid);}
static struct process*find_zombie_child(uint32_t parent_pid,uint32_t pid){for(uint32_t i=1;i<LIONOS_PROCESS_MAX;++i)if(is_child_of(&processes[i],parent_pid,pid)&&processes[i].state==PROCESS_ZOMBIE)return &processes[i];return 0;}
static int has_child(uint32_t parent_pid,uint32_t pid){for(uint32_t i=1;i<LIONOS_PROCESS_MAX;++i)if(is_child_of(&processes[i],parent_pid,pid))return 1;return 0;}
static void reap_process(struct process*p){
    if(!p||p->state!=PROCESS_ZOMBIE)return;
    if(p->is_thread){
        if(p->thread_stack_page){
            if(paging_unmap_user_page_in(p->page_directory,p->thread_stack_va,
                                         p->thread_stack_page)!=0)return;
            page_free((void *)(uintptr_t)p->thread_stack_page);
        }
        if(p->kernel_stack_top)
            page_free((void *)(uintptr_t)(p->kernel_stack_top-4096u));
        clear_process(p);
        return;
    }
    for(uint32_t i=0;i<p->user_page_count;++i)
        if(p->user_pages[i])page_free((void *)(uintptr_t)p->user_pages[i]);
    if(p->page_directory&&p->page_directory!=paging_kernel_directory())
        paging_destroy_address_space(p->page_directory);
    if(p->kernel_stack_top)
        page_free((void *)(uintptr_t)(p->kernel_stack_top-4096u));
    clear_process(p);
}
int process_get_exit_code(uint32_t pid,uint32_t*code){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process*p=0;
    for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i)if(processes[i].state!=PROCESS_UNUSED&&processes[i].pid==pid){p=&processes[i];break;}
    if(!p||p->state!=PROCESS_ZOMBIE){spinlock_irqrestore_release(&process_lock,flags);return -1;}
    if(code)*code=p->exit_code;
    spinlock_irqrestore_release(&process_lock,flags);
    return 0;
}
int process_reap_pid(uint32_t pid){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process*p=0;
    for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i)if(processes[i].state!=PROCESS_UNUSED&&processes[i].pid==pid){p=&processes[i];break;}
    if(!p||p->state!=PROCESS_ZOMBIE){spinlock_irqrestore_release(&process_lock,flags);return -1;}
    reap_process(p);
    spinlock_irqrestore_release(&process_lock,flags);
    return 0;
}

int32_t process_waitpid(uint32_t pid,uint32_t status_ptr){ uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*c=current_local(); if(!c||c==&processes[0]||!status_ptr){spinlock_irqrestore_release(&process_lock,flags);return -1;} struct process*z=find_zombie_child(c->pid,pid);if(z){if(write_user_u32(c,status_ptr,z->exit_code)!=0){spinlock_irqrestore_release(&process_lock,flags);return -1;}int32_t r=(int32_t)z->pid;reap_process(z);spinlock_irqrestore_release(&process_lock,flags);return r;} if(!has_child(c->pid,pid)){spinlock_irqrestore_release(&process_lock,flags);return -1;} c->wait_pid=pid;c->wait_status_ptr=status_ptr;c->wait_channel=0;c->wait_kind=0;c->state=PROCESS_WAITING;spinlock_irqrestore_release(&process_lock,flags);return PROCESS_WAIT_BLOCKED; }
static int32_t process_sleep_on_kind(uintptr_t channel, struct spinlock *held_lock,
                                      uint32_t irq_flags, uint32_t kind,
                                      uint32_t timeout_ticks){
    if(!channel||!held_lock||timeout_ticks>0x7FFFFFFFu){
        if(held_lock)spinlock_irqrestore_release(held_lock,irq_flags);
        return -1;
    }
    struct process *c=current_local();
    if(!c||c==&processes[0]||c->state==PROCESS_UNUSED){
        spinlock_irqrestore_release(held_lock,irq_flags);
        return -1;
    }
    /* Keep the object lock until PROCESS_WAITING is visible under process_lock.
       This closes the check-to-sleep lost-wakeup window. */
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    uint32_t cpu=cpu_current_index();
    if(cpu>=LIONOS_MAX_CPUS)cpu=0u;
    c->wait_channel=(uint32_t)channel;
    c->wait_kind=kind;
    c->wait_timeout_active=((kind==1u||kind==2u)&&timeout_ticks!=0u)?2u:0u;
    c->wait_timeout_result=0u;
    c->wait_timeout_ticks=timeout_ticks;
    c->wait_cpu=cpu;
    c->wait_deadline=0u;
    c->state=PROCESS_WAITING;
    spinlock_irqrestore_release(&process_lock,flags);
    spinlock_irqrestore_release(held_lock,irq_flags);
    return PROCESS_WAIT_BLOCKED;
}

int32_t process_sleep_on(uintptr_t channel, struct spinlock *held_lock, uint32_t irq_flags){
    return process_sleep_on_kind(channel,held_lock,irq_flags,0u,0u);
}

int32_t process_sleep_on_timeout(uintptr_t channel, struct spinlock *held_lock,
                                 uint32_t irq_flags, uint32_t timeout_ticks){
    return process_sleep_on_kind(channel,held_lock,irq_flags,2u,timeout_ticks);
}

int32_t process_consume_timeout(void){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *current=current_local();
    int32_t result=current&&current->wait_timeout_result?1:0;
    if(current)current->wait_timeout_result=0u;
    spinlock_irqrestore_release(&process_lock,flags);
    return result;
}

int32_t process_futex_sleep_on(uintptr_t channel, struct spinlock *held_lock, uint32_t irq_flags){
    return process_sleep_on_kind(channel,held_lock,irq_flags,1u,0u);
}

int32_t process_futex_sleep_on_timeout(uintptr_t channel, struct spinlock *held_lock,
                                       uint32_t irq_flags, uint32_t timeout_ticks){
    return process_sleep_on_kind(channel,held_lock,irq_flags,1u,timeout_ticks);
}

void process_futex_tick(uint32_t cpu){
    if(cpu>=LIONOS_MAX_CPUS)return;
    if(!spinlock_try_acquire(&process_lock))return;
    uint32_t now=++scheduler_cpu[cpu].timer_ticks;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *p=&processes[i];
        if(p->state!=PROCESS_WAITING||(p->wait_kind!=1u&&p->wait_kind!=2u)||
           p->wait_timeout_active!=1u||p->wait_cpu!=cpu||
           (int32_t)(now-p->wait_deadline)<0)continue;
        uint32_t kind=p->wait_kind;
        p->wait_channel=0u;
        p->wait_kind=0u;
        p->wait_timeout_active=0u;
        p->state=PROCESS_READY;
        if(kind==2u)p->wait_timeout_result=1u;
        if(kind==1u){
            uint32_t *frame=(uint32_t *)(uintptr_t)p->saved_frame;
            if(frame)frame[11]=2u;
        }
    }
    spinlock_release(&process_lock);
}

void process_wakeup(uintptr_t channel){
    if(!channel)return;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *p=&processes[i];
        if(p->state==PROCESS_WAITING&&(p->wait_kind==0u||p->wait_kind==2u)&&
           p->wait_channel==(uint32_t)channel){
            p->wait_channel=0u;
            p->wait_timeout_active=0u;
            p->wait_timeout_result=0u;
            p->wait_kind=0u;
            p->state=PROCESS_READY;
        }
    }
    spinlock_irqrestore_release(&process_lock,flags);
}

uint32_t process_futex_wakeup(uintptr_t channel,uint32_t maximum){
    if(!channel||!maximum)return 0u;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock),woken=0u;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX&&woken<maximum;++i){
        struct process *p=&processes[i];
        if(p->state!=PROCESS_WAITING||p->wait_kind!=1u||
           p->wait_channel!=(uint32_t)channel)continue;
        p->wait_channel=0u;
        p->wait_kind=0u;
        p->wait_timeout_active=0u;
        p->state=PROCESS_READY;
        uint32_t *frame=(uint32_t *)(uintptr_t)p->saved_frame;
        if(frame)frame[11]=0u;
        ++woken;
    }
    spinlock_irqrestore_release(&process_lock,flags);
    return woken;
}

static void wake_waiting_parent(struct process*child){for(uint32_t i=1;i<LIONOS_PROCESS_MAX;++i){struct process*p=&processes[i];if(p->state!=PROCESS_WAITING||p->pid!=child->parent_pid)continue;if(p->wait_pid!=PROCESS_WAIT_ANY&&p->wait_pid!=child->pid)continue;uint32_t*f=(uint32_t *)(uintptr_t)p->saved_frame;if(f)f[11]=child->pid;if(p->wait_status_ptr)write_user_u32(p,p->wait_status_ptr,child->exit_code);p->wait_pid=0;p->wait_status_ptr=0;p->wait_channel=0;p->state=PROCESS_READY;child->reap_pending=1;return;}}
void process_exit_current(uint32_t code){
    struct process*c=current_local();
    if(c&&!c->is_thread&&c!=&processes[0]){
        pipe_close_all(c);
        vfs_close_all_process(c);
    }
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    c=current_local();
    if(c&&c->is_thread){
        c->exit_code=code;
        c->state=PROCESS_ZOMBIE;
        for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
            struct process*w=&processes[i];
            if(w->state!=PROCESS_WAITING||w->is_thread||
               w->thread_group_pid!=c->thread_group_pid||w->join_tid!=c->tid)
                continue;
            uint32_t*f=(uint32_t*)(uintptr_t)w->saved_frame;
            int status_ok=w->join_status_ptr&&
                write_user_u32(w,w->join_status_ptr,code)==0;
            if(f)f[11]=status_ok?c->tid:PROCESS_SYSCALL_ERR;
            w->join_tid=0u;
            w->join_status_ptr=0u;
            w->state=PROCESS_READY;
            c->reap_pending=1u;
            break;
        }
        spinlock_irqrestore_release(&process_lock,flags);
        return;
    }
    if(!c||c==&processes[0]){
        spinlock_irqrestore_release(&process_lock,flags);
        return;
    }
    /* A process owns its user threads. Retire all non-current siblings before
       the address space can be reaped by waitpid. Threads share cpu_owner, so
       none of these siblings can be executing concurrently on another CPU. */
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process*t=&processes[i];
        if(t!=c&&t->state!=PROCESS_UNUSED&&t->is_thread&&
           t->thread_group_pid==c->thread_group_pid){
            t->state=PROCESS_ZOMBIE;
            reap_process(t);
        }
    }
    uint32_t exiting_sid=c->sid;
    c->exit_code=code;
    c->state=PROCESS_ZOMBIE;
    debug_write("LIONOS:PROC-EXIT\\n");
    wake_waiting_parent(c);
    spinlock_irqrestore_release(&process_lock,flags);
    process_release_tty_if_session_empty(exiting_sid);
}
static struct process*find_pid(uint32_t pid){for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i)if(processes[i].state!=PROCESS_UNUSED&&processes[i].pid==pid)return &processes[i];return 0;}
int process_is_descendant_or_child(uint32_t pid,uint32_t ancestor_pid){uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*p=find_pid(pid);if(!p||!ancestor_pid){spinlock_irqrestore_release(&process_lock,flags);return 0;}if(p->parent_pid==ancestor_pid){spinlock_irqrestore_release(&process_lock,flags);return 1;}for(uint32_t d=0;d<LIONOS_PROCESS_MAX&&p->parent_pid;++d){p=find_pid(p->parent_pid);if(!p)break;if(p->pid==ancestor_pid){spinlock_irqrestore_release(&process_lock,flags);return 1;}}spinlock_irqrestore_release(&process_lock,flags);return 0;}
static int process_is_descendant_locked(const struct process *candidate,uint32_t ancestor_pid){
    if(!candidate||!ancestor_pid)return 0;
    uint32_t parent_pid=candidate->parent_pid;
    for(uint32_t depth=0u;depth<LIONOS_PROCESS_MAX&&parent_pid;++depth){
        if(parent_pid==ancestor_pid)return 1;
        struct process *parent=find_pid(parent_pid);
        if(!parent)break;
        parent_pid=parent->parent_pid;
    }
    return 0;
}

int32_t process_tty_signal_foreground(uint32_t sid,uint32_t pgid,uint32_t signal){
    if(!sid||!pgid||signal!=LIONOS_SIG_INT)return LIONOS_SIGNAL_ERR;
    uint32_t bit=1u<<(signal-1u);
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    if(tty_get_foreground_pgid(sid)!=(int32_t)pgid){
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_ERR;
    }
    uint32_t members=0u;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *member=&processes[i];
        if(member->state==PROCESS_UNUSED||member->is_thread||
           member->state==PROCESS_ZOMBIE||member->pgid!=pgid)continue;
        if(member->sid!=sid){
            spinlock_irqrestore_release(&process_lock,flags);
            return LIONOS_SIGNAL_ERR;
        }
        ++members;
        for(uint32_t j=1u;j<LIONOS_PROCESS_MAX;++j){
            struct process *thread=&processes[j];
            if(thread->state==PROCESS_UNUSED||!thread->is_thread||
               thread->thread_group_pid!=member->pid||thread->state==PROCESS_ZOMBIE)continue;
            if(thread->sid!=sid){
                spinlock_irqrestore_release(&process_lock,flags);
                return LIONOS_SIGNAL_ERR;
            }
        }
    }
    if(!members){
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_ERR;
    }
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *member=&processes[i];
        if(member->state==PROCESS_UNUSED||member->is_thread||
           member->state==PROCESS_ZOMBIE||member->pgid!=pgid)continue;
        member->pending_signals|=bit;
        for(uint32_t j=1u;j<LIONOS_PROCESS_MAX;++j){
            struct process *thread=&processes[j];
            if(thread->state!=PROCESS_UNUSED&&thread->is_thread&&
               thread->thread_group_pid==member->pid&&thread->state!=PROCESS_ZOMBIE)
                thread->pending_signals|=bit;
        }
    }
    spinlock_irqrestore_release(&process_lock,flags);
    return LIONOS_SIGNAL_OK;
}

int32_t process_tty_check_foreground(uint32_t sid,uint32_t caller_pid,uint32_t signal){
    if(!sid||!caller_pid||
       (signal!=LIONOS_SIG_TTIN&&signal!=LIONOS_SIG_TTOU))
        return LIONOS_SIGNAL_ERR;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *caller=find_pid(caller_pid);
    if(caller&&caller->is_thread)
        caller=find_pid(caller->thread_group_pid);
    if(!caller||caller->state==PROCESS_ZOMBIE||caller->sid!=sid||
       !caller->pgid){
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_ERR;
    }
    int32_t foreground=tty_get_foreground_pgid(sid);
    if(foreground<=0){
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_ERR;
    }
    if(caller->pgid==(uint32_t)foreground){
        spinlock_irqrestore_release(&process_lock,flags);
        return 0;
    }
    uint32_t bit=1u<<(signal-1u);
    uint32_t members=0u;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *member=&processes[i];
        if(member->state==PROCESS_UNUSED||member->state==PROCESS_ZOMBIE||
           member->is_thread||member->pgid!=caller->pgid)
            continue;
        if(member->sid!=sid){
            spinlock_irqrestore_release(&process_lock,flags);
            return LIONOS_SIGNAL_ERR;
        }
        ++members;
        for(uint32_t j=1u;j<LIONOS_PROCESS_MAX;++j){
            struct process *thread=&processes[j];
            if(thread->state==PROCESS_UNUSED||thread->state==PROCESS_ZOMBIE||
               !thread->is_thread||thread->thread_group_pid!=member->pid)
                continue;
            if(thread->sid!=sid){
                spinlock_irqrestore_release(&process_lock,flags);
                return LIONOS_SIGNAL_ERR;
            }
        }
    }
    if(!members){
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_ERR;
    }
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *member=&processes[i];
        if(member->state==PROCESS_UNUSED||member->state==PROCESS_ZOMBIE||
           member->is_thread||member->pgid!=caller->pgid)
            continue;
        member->pending_signals|=bit;
        for(uint32_t j=1u;j<LIONOS_PROCESS_MAX;++j){
            struct process *thread=&processes[j];
            if(thread->state!=PROCESS_UNUSED&&thread->state!=PROCESS_ZOMBIE&&
               thread->is_thread&&thread->thread_group_pid==member->pid)
                thread->pending_signals|=bit;
        }
    }
    spinlock_irqrestore_release(&process_lock,flags);
    return 1;
}

int32_t process_signal_group(uint32_t pgid,uint32_t signal,uint32_t sender_pid){
    uint32_t targets[LIONOS_PROCESS_MAX];
    uint32_t count=0u;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *sender=sender_pid?find_pid(sender_pid):current_local();
    if(sender&&sender->is_thread)sender=find_pid(sender->thread_group_pid);
    if(!sender||sender==&processes[0]||!pgid||signal==0u||signal>LIONOS_SIG_MAX){
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_ERR;
    }
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *member=&processes[i];
        if(member->state==PROCESS_UNUSED||member->is_thread||
           member->state==PROCESS_ZOMBIE||member->pgid!=pgid)continue;
        if(member==sender||!process_is_descendant_locked(member,sender->pid)){
            spinlock_irqrestore_release(&process_lock,flags);
            return LIONOS_SIGNAL_ERR;
        }
        if(signal==LIONOS_SIG_KILL||signal==LIONOS_SIG_TERM){
            for(uint32_t j=1u;j<LIONOS_PROCESS_MAX;++j){
                struct process *thread=&processes[j];
                if(thread->state!=PROCESS_UNUSED&&thread->is_thread&&
                   thread->thread_group_pid==member->pid&&thread->state!=PROCESS_ZOMBIE){
                    spinlock_irqrestore_release(&process_lock,flags);
                    return LIONOS_SIGNAL_ERR;
                }
            }
        }
        if(count<LIONOS_PROCESS_MAX)targets[count++]=member->pid;
    }
    spinlock_irqrestore_release(&process_lock,flags);
    if(!count)return LIONOS_SIGNAL_ERR;
    uint32_t delivered=0u;
    for(uint32_t i=0u;i<count;++i)
        if(process_signal(targets[i],signal)==LIONOS_SIGNAL_OK)++delivered;
    return delivered?LIONOS_SIGNAL_OK:LIONOS_SIGNAL_ERR;
}

int32_t process_signal(uint32_t pid,uint32_t signal){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process*p=find_pid(pid);
    if(!p||p==&processes[0]||p->state==PROCESS_ZOMBIE||signal==0||signal>LIONOS_SIG_MAX){
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_ERR;
    }
    if(signal==LIONOS_SIG_KILL||signal==LIONOS_SIG_TERM){
        if(!p->is_thread){
            for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
                struct process *thread=&processes[i];
                if(thread->state!=PROCESS_UNUSED&&thread->is_thread&&
                   thread->thread_group_pid==p->pid&&thread->state!=PROCESS_ZOMBIE){
                    spinlock_irqrestore_release(&process_lock,flags);
                    return LIONOS_SIGNAL_ERR;
                }
            }
        }
        p->wait_channel=0u;
        p->wait_kind=0u;
        p->wait_timeout_active=0u;
        p->wait_timeout_result=0u;
        if(p->is_thread){
            p->pending_signals|=(1u<<(signal-1u));
            p->exit_code=128u+signal;
            p->state=PROCESS_ZOMBIE;
            spinlock_irqrestore_release(&process_lock,flags);
            return LIONOS_SIGNAL_OK;
        }
        p->pending_signals|=(1u<<(signal-1u));
        p->exit_code=128u+signal;
        p->state=PROCESS_STOPPED;
        uint32_t exiting_sid=p->sid;
        spinlock_irqrestore_release(&process_lock,flags);
        pipe_close_all(p);
        vfs_close_all_process(p);
        flags=spinlock_irqsave_acquire(&process_lock);
        if(p->state==PROCESS_STOPPED){
            p->state=PROCESS_ZOMBIE;
            wake_waiting_parent(p);
        }
        spinlock_irqrestore_release(&process_lock,flags);
        process_release_tty_if_session_empty(exiting_sid);
        return LIONOS_SIGNAL_OK;
    }
    if(signal==LIONOS_SIG_STOP){
        p->pending_signals|=(1u<<(signal-1u));
        if(p->state==PROCESS_WAITING&&(p->wait_kind==1u||p->wait_kind==2u)){
            uint32_t *frame=(uint32_t *)(uintptr_t)p->saved_frame;
            if(frame)frame[11]=LIONOS_FUTEX_INTERRUPTED;
            p->wait_channel=0u;p->wait_kind=0u;p->wait_timeout_active=0u;p->wait_timeout_result=0u;
        }
        p->state=PROCESS_STOPPED;
        if(!p->is_thread){
            for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
                struct process *thread=&processes[i];
                if(thread->state==PROCESS_UNUSED||thread->state==PROCESS_ZOMBIE||
                   !thread->is_thread||thread->thread_group_pid!=p->pid)continue;
                thread->pending_signals|=(1u<<(signal-1u));
                if(thread->state==PROCESS_WAITING&&(thread->wait_kind==1u||thread->wait_kind==2u)){
                    uint32_t *frame=(uint32_t *)(uintptr_t)thread->saved_frame;
                    if(frame)frame[11]=LIONOS_FUTEX_INTERRUPTED;
                    thread->wait_channel=0u;thread->wait_kind=0u;thread->wait_timeout_active=0u;thread->wait_timeout_result=0u;
                }
                thread->state=PROCESS_STOPPED;
                uint32_t cpu=thread->cpu_owner<LIONOS_MAX_CPUS?thread->cpu_owner:0u;
                __atomic_store_n(&reschedule_requested[cpu],1u,__ATOMIC_RELEASE);
            }
        }
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_OK;
    }
    if(signal==LIONOS_SIG_CONT){
        p->pending_signals&=~(1u<<(LIONOS_SIG_STOP-1u));
        if(p->state==PROCESS_STOPPED)p->state=PROCESS_READY;
        if(!p->is_thread){
            for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
                struct process *thread=&processes[i];
                if(thread->state==PROCESS_UNUSED||thread->state==PROCESS_ZOMBIE||
                   !thread->is_thread||thread->thread_group_pid!=p->pid)continue;
                thread->pending_signals&=~(1u<<(LIONOS_SIG_STOP-1u));
                if(thread->state==PROCESS_STOPPED)thread->state=PROCESS_READY;
                uint32_t cpu=thread->cpu_owner<LIONOS_MAX_CPUS?thread->cpu_owner:0u;
                __atomic_store_n(&reschedule_requested[cpu],1u,__ATOMIC_RELEASE);
            }
        }
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_OK;
    }
    spinlock_irqrestore_release(&process_lock,flags);
    return LIONOS_SIGNAL_ERR;
}
uint32_t process_signal_pending(uint32_t pid){uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*p=find_pid(pid);uint32_t r=p?p->pending_signals:0u;spinlock_irqrestore_release(&process_lock,flags);return r;}
int32_t process_get_state(uint32_t pid){uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*p=find_pid(pid);int32_t r=p?(int32_t)p->state:-1;spinlock_irqrestore_release(&process_lock,flags);return r;}
int process_has_capability(const struct process *p,uint32_t capability){return p&&(p->capabilities&capability)==capability;}
uint32_t process_capabilities(const struct process *p){return p?p->capabilities:0u;}
int process_set_capabilities(struct process *p,uint32_t capabilities){uint32_t flags=spinlock_irqsave_acquire(&process_lock);if(!p||p==&processes[0]){spinlock_irqrestore_release(&process_lock,flags);return -1;}p->capabilities=capabilities&~PROCESS_CAP_ADMIN;spinlock_irqrestore_release(&process_lock,flags);return 0;}

int process_set_current(struct process*p){uint32_t flags=spinlock_irqsave_acquire(&process_lock);if(!p||p->state==PROCESS_UNUSED||p->state==PROCESS_STOPPED||!p->page_directory){spinlock_irqrestore_release(&process_lock,flags);return -1;}struct process*old=current_local();if(old&&old!=p&&old->state==PROCESS_RUNNING)old->state=PROCESS_READY;set_current_local(p);p->cpu_owner=cpu_current_index();p->state=PROCESS_RUNNING;paging_switch_address_space(p->page_directory);tss_set_kernel_stack(p->kernel_stack_top);spinlock_irqrestore_release(&process_lock,flags);return 0;}
uint32_t process_count(void){uint32_t flags=spinlock_irqsave_acquire(&process_lock),n=0;for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i)if(processes[i].state!=PROCESS_UNUSED)++n;spinlock_irqrestore_release(&process_lock,flags);return n;}
void process_set_saved_frame(struct process*p,uint32_t*f){uint32_t flags=spinlock_irqsave_acquire(&process_lock);if(p)p->saved_frame=(uint32_t)(uintptr_t)f;spinlock_irqrestore_release(&process_lock,flags);}
uint32_t*process_saved_frame(struct process*p){return p?(uint32_t *)(uintptr_t)p->saved_frame:0;}
uint32_t process_kernel_stack_top(struct process*p){return p?p->kernel_stack_top:0;}

static uint32_t thread_stack_va_for_slot(uint32_t slot){return PROCESS_THREAD_STACK_BASE-(slot*PROCESS_THREAD_STACK_STRIDE)-0x1000u;}
struct process *process_create_kernel_thread(uint32_t entry,uint32_t arg){
    if(!entry)return 0;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process*p=find_free_slot();if(!p){spinlock_irqrestore_release(&process_lock,flags);return 0;}
    void*ks=page_alloc();if(!ks){spinlock_irqrestore_release(&process_lock,flags);return 0;}
    clear_process(p);p->pid=0;p->tid=next_tid++;if(next_tid==0)next_tid=1;p->thread_group_pid=0;p->is_thread=1;p->owns_address_space=0;p->state=PROCESS_READY;p->cpu_owner=cpu_current_index();p->page_directory=paging_kernel_directory();p->kernel_stack_top=(uint32_t)(uintptr_t)ks+4096u;
    uint32_t*f=(uint32_t*)(uintptr_t)(p->kernel_stack_top-PROCESS_CONTEXT_WORDS*4u);for(uint32_t i=0;i<PROCESS_CONTEXT_WORDS;++i)f[i]=0;
    f[14]=entry;f[15]=0x08u;f[16]=0x202u;f[17]=p->kernel_stack_top-64u;f[18]=0x10u;
    ((uint32_t*)(uintptr_t)(p->kernel_stack_top-64u))[0]=arg;p->saved_frame=(uint32_t)(uintptr_t)f;
    spinlock_irqrestore_release(&process_lock,flags);return p;
}
uint32_t process_create_user_thread(uint32_t entry){
    if(!entry)return PROCESS_SYSCALL_ERR;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*parent=current_local();
    if(!parent||parent==&processes[0]||
       !paging_user_page_executable(parent->page_directory,entry)){
        spinlock_irqrestore_release(&process_lock,flags);return PROCESS_SYSCALL_ERR;
    }
    struct process*p=find_free_slot();if(!p){spinlock_irqrestore_release(&process_lock,flags);return PROCESS_SYSCALL_ERR;}
    void*kp=page_alloc();void*ks=page_alloc();if(!kp||!ks){if(kp)page_free(kp);if(ks)page_free(ks);spinlock_irqrestore_release(&process_lock,flags);return PROCESS_SYSCALL_ERR;}
    uint32_t slot=process_index(p),va=thread_stack_va_for_slot(slot);
    if(paging_map_user_page_in(parent->page_directory,va,(uint32_t)(uintptr_t)kp,3u)!=0){page_free(kp);page_free(ks);spinlock_irqrestore_release(&process_lock,flags);return PROCESS_SYSCALL_ERR;}
    for(uint32_t i=0;i<4096u;++i)((uint8_t*)kp)[i]=0;
    uint32_t top=va+4096u;uint32_t*sp=(uint32_t*)(uintptr_t)top;--sp;*sp=0u;
    clear_process(p);p->pid=0;p->tid=next_tid++;if(next_tid==0)next_tid=1;p->thread_group_pid=parent->thread_group_pid;p->pgid=parent->pgid;p->sid=parent->sid;p->is_thread=1;p->owns_address_space=0;p->state=PROCESS_READY;p->cpu_owner=cpu_current_index();p->page_directory=parent->page_directory;p->entry=entry;p->user_stack=(uint32_t)(uintptr_t)sp;p->kernel_stack_top=(uint32_t)(uintptr_t)ks+4096u;p->thread_stack_page=(uint32_t)(uintptr_t)kp;p->thread_stack_va=va;p->capabilities=parent->capabilities;p->priority=parent->priority;p->base_priority=parent->base_priority;p->time_slice=quantum_for_priority(p->priority);p->fpu_valid=0;
    uint32_t*f=(uint32_t*)(uintptr_t)(p->kernel_stack_top-PROCESS_CONTEXT_WORDS*4u);for(uint32_t i=0;i<PROCESS_CONTEXT_WORDS;++i)f[i]=0;f[14]=entry;f[15]=USER_CODE_SEL;f[16]=0x202u;f[17]=p->user_stack;f[18]=USER_DATA_SEL;p->saved_frame=(uint32_t)(uintptr_t)f;
    spinlock_irqrestore_release(&process_lock,flags);return p->tid;
}
void process_exit_thread(uint32_t code){process_exit_current(code);}
int32_t process_join_thread(uint32_t tid,uint32_t status_ptr){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*c=current_local();if(!c||c->is_thread||!tid||!status_ptr){spinlock_irqrestore_release(&process_lock,flags);return -1;}
    for(uint32_t i=1;i<LIONOS_PROCESS_MAX;++i){struct process*p=&processes[i];if(!p->is_thread||p->thread_group_pid!=c->thread_group_pid||p->tid!=tid)continue;
        if(p->state==PROCESS_ZOMBIE){if(write_user_u32(c,status_ptr,p->exit_code)!=0){spinlock_irqrestore_release(&process_lock,flags);return -1;}reap_process(p);spinlock_irqrestore_release(&process_lock,flags);return (int32_t)tid;}
        c->join_tid=tid;c->join_status_ptr=status_ptr;c->state=PROCESS_WAITING;spinlock_irqrestore_release(&process_lock,flags);return PROCESS_WAIT_BLOCKED;
    }
    spinlock_irqrestore_release(&process_lock,flags);return -1;
}

void process_request_reschedule(void){
    uint32_t cpu=cpu_current_index();
    if(cpu>=LIONOS_MAX_CPUS)cpu=0u;
    __atomic_store_n(&reschedule_requested[cpu],1u,__ATOMIC_RELEASE);
}

int process_set_priority(struct process*p,uint32_t priority){
    if(!p||priority>=PROCESS_PRIORITY_LEVELS)return -1;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    p->priority=priority;p->base_priority=priority;p->time_slice=quantum_for_priority(priority);p->ticks_used=0;
    spinlock_irqrestore_release(&process_lock,flags);
    return 0;
}
uint32_t process_scheduler_ticks(uint32_t cpu){return cpu<LIONOS_MAX_CPUS?scheduler_cpu[cpu].schedule_count:0u;}
uint32_t process_scheduler_steals(uint32_t cpu){return cpu<LIONOS_MAX_CPUS?scheduler_cpu[cpu].steal_count:0u;}

uint32_t *process_schedule(uint32_t *frame){
    /* Do not let a timer interrupt spin behind another CPU. Linux keeps\n     * scheduler critical sections CPU-local; until LionOS has independent\n     * runqueue locks, dropping a contended tick is preferable to stalling\n     * the CPU in an IRQ-disabled spin loop. */
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    if(!spinlock_try_acquire(&process_lock)){
        __asm__ volatile("pushl %0; popfl" : : "r"(flags) : "memory","cc");
        return frame;
    }
    uint32_t cpu=cpu_current_index();
    if(cpu>=LIONOS_MAX_CPUS)cpu=0u;
    struct scheduler_cpu_state*sc=&scheduler_cpu[cpu];
    ++sc->schedule_count;
    struct process*prev=current_local();
    if(prev){
        prev->saved_frame=(uint32_t)(uintptr_t)frame;
        if(prev->state==PROCESS_WAITING&&(prev->wait_kind==1u||prev->wait_kind==2u)&&
           prev->wait_timeout_active==2u&&prev->wait_cpu==cpu){
            prev->wait_deadline=sc->timer_ticks+prev->wait_timeout_ticks;
            prev->wait_timeout_active=1u;
        }
    }

    if(prev && prev->state==PROCESS_RUNNING){
        if(__atomic_exchange_n(&reschedule_requested[cpu],0u,__ATOMIC_ACQ_REL)==0u){
            ++prev->ticks_used;
            if(prev->ticks_used < prev->time_slice){
                spinlock_irqrestore_release(&process_lock,flags);
                return frame;
            }
            prev->ticks_used=0u;
            if(prev->priority+1u<PROCESS_PRIORITY_LEVELS) ++prev->priority;
        } else {
            prev->ticks_used=0u;
        }
    }
    if(prev&&prev->deferred_kernel_stack){page_free((void *)(uintptr_t)(prev->deferred_kernel_stack-4096u));prev->deferred_kernel_stack=0;}

    uint32_t start_idx=prev?process_index(prev):sc->cursor;
    struct process*next=0;
    /* Per-CPU run queues are represented by cpu_owner. Within each queue,
       select the highest-priority READY task, then round-robin among peers. */
    for(uint32_t priority=0u;priority<PROCESS_PRIORITY_LEVELS && !next;++priority){
        for(uint32_t step=1u;step<=LIONOS_PROCESS_MAX;++step){
            uint32_t idx=(start_idx+step)%LIONOS_PROCESS_MAX;
            struct process*p=&processes[idx];
            if(p->state==PROCESS_READY&&p->cpu_owner==cpu&&p->priority==priority){
                next=p;sc->cursor=idx;break;
            }
        }
    }
    /*
     * Cross-CPU work stealing is temporarily disabled. The AP currently lacks
     * a dedicated scheduler idle context for safely retiring a task that exits
     * on a secondary CPU. Keep each task on its creator CPU until that context
     * and migration-safe run-queue handling are implemented.
     */
    if(next){
        /* Preserve the outgoing CPU's FPU/SSE state on every real switch,
           including tasks that blocked, stopped, or exited before scheduling. */
        if(prev)fpu_save(prev);
        if(prev&&prev->state==PROCESS_RUNNING)prev->state=PROCESS_READY;
        fpu_restore(next);
        set_current_local(next);
        next->cpu_owner=cpu;
        next->state=PROCESS_RUNNING;
        /* Scheduler hot path: never emit debug I/O here. Port 0xE9 is a\n         * QEMU debug console and is far more expensive than the context switch. */
        paging_switch_address_space(next->page_directory);
        tss_set_kernel_stack(next->kernel_stack_top);
        if(prev&&prev->reap_pending)reap_process(prev);
        uint32_t*n=process_saved_frame(next);
        spinlock_irqrestore_release(&process_lock,flags);
        return n;
    }
    if(prev&&(prev->state==PROCESS_ZOMBIE||prev->state==PROCESS_STOPPED||prev->state==PROCESS_WAITING)){
        if(cpu==0){
            fpu_save(prev);
            struct process*b=&processes[0];
            set_current_local(b);
            b->cpu_owner=0;
            b->state=PROCESS_RUNNING;
            paging_switch_address_space(b->page_directory);
            tss_set_kernel_stack(b->kernel_stack_top);
            if(prev->reap_pending)reap_process(prev);
            uint32_t*n=process_saved_frame(b);
            spinlock_irqrestore_release(&process_lock,flags);
            return n;
        }
        set_current_local(0);
        if(prev->reap_pending)reap_process(prev);
        spinlock_irqrestore_release(&process_lock,flags);
        return frame;
    }
    if(!prev&&cpu==0){
        set_current_local(&processes[0]);
        processes[0].cpu_owner=0;
        processes[0].state=PROCESS_RUNNING;
        paging_switch_address_space(processes[0].page_directory);
        tss_set_kernel_stack(processes[0].kernel_stack_top);
    }
    spinlock_irqrestore_release(&process_lock,flags);
    return frame;
}
void scheduler_idle(void){for(;;)__asm__ volatile("sti; hlt");}
