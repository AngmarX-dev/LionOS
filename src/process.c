#include <stdint.h>
#include "memory.h"
#include "paging.h"
#include "process.h"
#include "signal.h"
#include "tss.h"
#include "spinlock.h"
#include "cpu.h"

#define PROCESS_WAIT_ANY 0u
#define PROCESS_WAIT_BLOCKED (-2)
#define PROCESS_SYSCALL_ERR 0xFFFFFFFFu
#define PAGE_SIZE 4096u
#define USER_CODE_SEL 0x2Bu
#define USER_DATA_SEL 0x33u

static struct process processes[LIONOS_PROCESS_MAX];
static struct process *current_by_cpu[LIONOS_MAX_CPUS];
static uint32_t next_pid;
static struct spinlock process_lock;

static struct process *current_local(void) {
    uint32_t cpu = cpu_current_index();
    return cpu < LIONOS_MAX_CPUS ? current_by_cpu[cpu] : 0;
}
static void set_current_local(struct process *p) {
    uint32_t cpu = cpu_current_index();
    if (cpu < LIONOS_MAX_CPUS) current_by_cpu[cpu] = p;
}
static struct process *find_free_slot(void) { for (uint32_t i=0;i<LIONOS_PROCESS_MAX;++i) if (processes[i].state==PROCESS_UNUSED) return &processes[i]; return 0; }
static uint32_t process_index(const struct process *p) { return (uint32_t)(p-processes); }
static int write_user_u32(const struct process *p,uint32_t ptr,uint32_t value) {
    uint32_t physical,flags;
    if (!p||!ptr||ptr>0xBFFFFFFCu||(ptr&3u)) return -1;
    if (paging_get_user_page(p->page_directory,ptr&~(PAGE_SIZE-1u),&physical,&flags)!=0) return -1;
    (void)flags;
    *(uint32_t *)(uintptr_t)(physical+(ptr&(PAGE_SIZE-1u)))=value;
    return 0;
}
static void clear_process(struct process *p) {
    p->pid=0;p->state=PROCESS_UNUSED;p->parent_pid=0;p->exit_code=0;p->wait_pid=0;p->wait_status_ptr=0;
    p->reap_pending=0;p->deferred_kernel_stack=0;p->page_directory=0;p->entry=0;p->user_stack=0;
    p->kernel_stack_top=0;p->saved_frame=0;p->user_code_page=0;p->user_stack_page=0;p->user_page_count=0;p->pending_signals=0;
    for(uint32_t i=0;i<LIONOS_PROCESS_MAX_USER_PAGES;++i){p->user_pages[i]=0;p->user_page_vas[i]=0;}
}
static void init_interrupt_frame(struct process *p) {
    uint32_t *f=(uint32_t *)(uintptr_t)(p->kernel_stack_top-PROCESS_CONTEXT_WORDS*4u);
    for(uint32_t i=0;i<PROCESS_CONTEXT_WORDS;++i) f[i]=0;
    f[0]=USER_DATA_SEL;f[1]=USER_DATA_SEL;f[2]=USER_DATA_SEL;f[3]=USER_DATA_SEL;
    f[14]=p->entry;f[15]=USER_CODE_SEL;f[16]=0x202u;f[17]=p->user_stack;f[18]=USER_DATA_SEL;
    p->saved_frame=(uint32_t)(uintptr_t)f;
}
static void init_bootstrap_frame(void) {
    struct process *b=&processes[0];
    void *s=page_alloc();
    if(!s){b->kernel_stack_top=0;b->saved_frame=0;return;}
    b->kernel_stack_top=(uint32_t)(uintptr_t)s+4096u;b->page_directory=paging_kernel_directory();
    b->entry=(uint32_t)(uintptr_t)&process_schedule;b->user_stack=b->kernel_stack_top;
    uint32_t *f=(uint32_t *)(uintptr_t)(b->kernel_stack_top-PROCESS_CONTEXT_WORDS*4u);
    for(uint32_t i=0;i<PROCESS_CONTEXT_WORDS;++i) f[i]=0;
    f[0]=0x10u;f[1]=0x10u;f[2]=0x10u;f[3]=0x10u;f[14]=(uint32_t)(uintptr_t)&scheduler_idle;
    f[15]=0x08u;f[16]=0x202u;f[17]=b->kernel_stack_top;f[18]=0x10u;
    b->saved_frame=(uint32_t)(uintptr_t)f;
}
void process_init(void) {
    spinlock_init(&process_lock);
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i) clear_process(&processes[i]);
    for(uint32_t i=0;i<LIONOS_MAX_CPUS;++i) current_by_cpu[i]=0;
    next_pid=2;processes[0].pid=1;processes[0].state=PROCESS_RUNNING;init_bootstrap_frame();
    set_current_local(&processes[0]);
    spinlock_irqrestore_release(&process_lock,flags);
}
struct process *process_current(void){return current_local();}
struct process *process_at(uint32_t i){return i<LIONOS_PROCESS_MAX?&processes[i]:0;}
uint32_t process_current_pid(void){struct process*p=current_local();return p?p->pid:0;}
const char *process_state_name(uint32_t s){switch(s){case PROCESS_READY:return "READY";case PROCESS_RUNNING:return "RUNNING";case PROCESS_ZOMBIE:return "ZOMBIE";case PROCESS_WAITING:return "WAITING";case PROCESS_STOPPED:return "STOPPED";default:return "UNUSED";}}

static struct process *process_create_ex_vas_locked(uint32_t entry,uint32_t stack,uint32_t pd,const uint32_t *pages,const uint32_t *vas,uint32_t count){
    struct process*p=find_free_slot();
    if(!p||!pd||!pages||!vas||!count||count>LIONOS_PROCESS_MAX_USER_PAGES)return 0;
    void *ks=page_alloc();if(!ks)return 0;
    p->pid=next_pid++;if(next_pid==0)next_pid=2;p->state=PROCESS_READY;
    struct process *parent=current_local();p->parent_pid=parent?parent->pid:1;
    p->exit_code=0;p->wait_pid=0;p->wait_status_ptr=0;p->reap_pending=0;p->deferred_kernel_stack=0;
    p->page_directory=pd;p->entry=entry;p->user_stack=stack;p->kernel_stack_top=(uint32_t)(uintptr_t)ks+4096u;
    p->saved_frame=0;p->user_page_count=count;p->user_code_page=pages[0];p->user_stack_page=pages[count-1u];p->pending_signals=0;
    for(uint32_t i=0;i<count;++i){p->user_pages[i]=pages[i];p->user_page_vas[i]=vas[i];}
    for(uint32_t i=count;i<LIONOS_PROCESS_MAX_USER_PAGES;++i){p->user_pages[i]=0;p->user_page_vas[i]=0;}
    init_interrupt_frame(p);return p;
}
struct process *process_create_ex_vas(uint32_t entry,uint32_t stack,uint32_t pd,const uint32_t *pages,const uint32_t *vas,uint32_t count){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*p=process_create_ex_vas_locked(entry,stack,pd,pages,vas,count);spinlock_irqrestore_release(&process_lock,flags);return p;
}
struct process *process_create_ex(uint32_t entry,uint32_t stack,uint32_t pd,const uint32_t *pages,uint32_t count){
    uint32_t vas[LIONOS_PROCESS_MAX_USER_PAGES];for(uint32_t i=0;i<count&&i<LIONOS_PROCESS_MAX_USER_PAGES;++i)vas[i]=i*4096u;return process_create_ex_vas(entry,stack,pd,pages,vas,count);
}
struct process *process_create(uint32_t entry,uint32_t stack,uint32_t pd,uint32_t code,uint32_t user_stack_page){
    uint32_t p[2]={code,user_stack_page},v[2]={0x00400000u,0x00401000u};return process_create_ex_vas(entry,stack,pd,p,v,2u);
}

int process_exec_replace_current(uint32_t entry,uint32_t stack,uint32_t pd,const uint32_t *pages,const uint32_t *vas,uint32_t count){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*c=current_local();
    if(!c||c==&processes[0]||!pd||!pages||!vas||!count||count>LIONOS_PROCESS_MAX_USER_PAGES){spinlock_irqrestore_release(&process_lock,flags);return -1;}
    void*new_ks=page_alloc();if(!new_ks){spinlock_irqrestore_release(&process_lock,flags);return -1;}
    uint32_t old_pd=c->page_directory,old_ks=c->kernel_stack_top,old_pages[LIONOS_PROCESS_MAX_USER_PAGES],old_count=c->user_page_count;
    for(uint32_t i=0;i<old_count;++i)old_pages[i]=c->user_pages[i];
    c->page_directory=pd;c->entry=entry;c->user_stack=stack;c->kernel_stack_top=(uint32_t)(uintptr_t)new_ks+4096u;c->saved_frame=0;
    c->user_page_count=count;c->user_code_page=pages[0];c->user_stack_page=pages[count-1u];c->exit_code=0;c->wait_pid=0;c->wait_status_ptr=0;c->reap_pending=0;c->pending_signals=0;c->deferred_kernel_stack=old_ks;
    for(uint32_t i=0;i<count;++i){c->user_pages[i]=pages[i];c->user_page_vas[i]=vas[i];}
    for(uint32_t i=count;i<LIONOS_PROCESS_MAX_USER_PAGES;++i){c->user_pages[i]=0;c->user_page_vas[i]=0;}
    paging_switch_address_space(pd);tss_set_kernel_stack(c->kernel_stack_top);init_interrupt_frame(c);
    for(uint32_t i=0;i<old_count;++i)if(old_pages[i])page_free((void *)(uintptr_t)old_pages[i]);
    if(old_pd&&old_pd!=pd)paging_destroy_address_space(old_pd);
    spinlock_irqrestore_release(&process_lock,flags);return 0;
}

uint32_t process_fork_current(uint32_t *parent_frame){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*parent=current_local();
    if(!parent||parent==&processes[0]||!parent_frame||!parent->user_page_count){spinlock_irqrestore_release(&process_lock,flags);return PROCESS_SYSCALL_ERR;}
    struct process*slot=find_free_slot();if(!slot){spinlock_irqrestore_release(&process_lock,flags);return PROCESS_SYSCALL_ERR;}
    uint32_t pd=paging_create_address_space();if(!pd){spinlock_irqrestore_release(&process_lock,flags);return PROCESS_SYSCALL_ERR;}
    uint32_t copied[LIONOS_PROCESS_MAX_USER_PAGES]={0},count=parent->user_page_count;
    for(uint32_t i=0;i<count;++i){uint32_t src,pflags;if(paging_get_user_page(parent->page_directory,parent->user_page_vas[i],&src,&pflags)!=0)goto fail;void*dst=page_alloc();if(!dst)goto fail;copied[i]=(uint32_t)(uintptr_t)dst;for(uint32_t b=0;b<4096u;++b)((uint8_t*)dst)[b]=((const uint8_t*)(uintptr_t)src)[b];if(paging_map_user_page_in(pd,parent->user_page_vas[i],copied[i],pflags)!=0)goto fail;}
    slot=process_create_ex_vas_locked(parent->entry,parent->user_stack,pd,copied,parent->user_page_vas,count);if(!slot)goto fail;
    slot->parent_pid=parent->pid;{uint32_t*cf=(uint32_t *)(uintptr_t)slot->saved_frame;for(uint32_t i=0;i<PROCESS_CONTEXT_WORDS;++i)cf[i]=parent_frame[i];cf[11]=0;}
    uint32_t result=slot->pid;spinlock_irqrestore_release(&process_lock,flags);return result;
fail:for(uint32_t i=0;i<count;++i)if(copied[i])page_free((void *)(uintptr_t)copied[i]);paging_destroy_address_space(pd);spinlock_irqrestore_release(&process_lock,flags);return PROCESS_SYSCALL_ERR;
}

static int is_child_of(const struct process*c,uint32_t parent_pid,uint32_t pid){return c->state!=PROCESS_UNUSED&&c->parent_pid==parent_pid&&(pid==PROCESS_WAIT_ANY||c->pid==pid);}
static struct process*find_zombie_child(uint32_t parent_pid,uint32_t pid){for(uint32_t i=1;i<LIONOS_PROCESS_MAX;++i)if(is_child_of(&processes[i],parent_pid,pid)&&processes[i].state==PROCESS_ZOMBIE)return &processes[i];return 0;}
static int has_child(uint32_t parent_pid,uint32_t pid){for(uint32_t i=1;i<LIONOS_PROCESS_MAX;++i)if(is_child_of(&processes[i],parent_pid,pid))return 1;return 0;}
static void reap_process(struct process*p){if(!p||p->state!=PROCESS_ZOMBIE)return;for(uint32_t i=0;i<p->user_page_count;++i)if(p->user_pages[i])page_free((void *)(uintptr_t)p->user_pages[i]);if(p->page_directory&&p->page_directory!=paging_kernel_directory())paging_destroy_address_space(p->page_directory);if(p->kernel_stack_top)page_free((void *)(uintptr_t)(p->kernel_stack_top-4096u));clear_process(p);}
int32_t process_waitpid(uint32_t pid,uint32_t status_ptr){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*c=current_local();
    if(!c||c==&processes[0]||!status_ptr){spinlock_irqrestore_release(&process_lock,flags);return -1;}
    struct process*z=find_zombie_child(c->pid,pid);if(z){if(write_user_u32(c,status_ptr,z->exit_code)!=0){spinlock_irqrestore_release(&process_lock,flags);return -1;}int32_t r=(int32_t)z->pid;reap_process(z);spinlock_irqrestore_release(&process_lock,flags);return r;}
    if(!has_child(c->pid,pid)){spinlock_irqrestore_release(&process_lock,flags);return -1;}
    c->wait_pid=pid;c->wait_status_ptr=status_ptr;c->state=PROCESS_WAITING;spinlock_irqrestore_release(&process_lock,flags);return PROCESS_WAIT_BLOCKED;
}
static void wake_waiting_parent(struct process*child){for(uint32_t i=1;i<LIONOS_PROCESS_MAX;++i){struct process*p=&processes[i];if(p->state!=PROCESS_WAITING||p->pid!=child->parent_pid)continue;if(p->wait_pid!=PROCESS_WAIT_ANY&&p->wait_pid!=child->pid)continue;uint32_t*f=(uint32_t *)(uintptr_t)p->saved_frame;if(f)f[11]=child->pid;if(p->wait_status_ptr)write_user_u32(p,p->wait_status_ptr,child->exit_code);p->wait_pid=0;p->wait_status_ptr=0;p->state=PROCESS_READY;child->reap_pending=1;return;}}
void process_exit_current(uint32_t code){uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*c=current_local();if(!c||c==&processes[0]){spinlock_irqrestore_release(&process_lock,flags);return;}c->exit_code=code;c->state=PROCESS_ZOMBIE;wake_waiting_parent(c);spinlock_irqrestore_release(&process_lock,flags);}
static struct process*find_pid(uint32_t pid){for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i)if(processes[i].state!=PROCESS_UNUSED&&processes[i].pid==pid)return &processes[i];return 0;}
int process_is_descendant_or_child(uint32_t pid,uint32_t ancestor_pid){uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*p=find_pid(pid);if(!p||!ancestor_pid){spinlock_irqrestore_release(&process_lock,flags);return 0;}if(p->parent_pid==ancestor_pid){spinlock_irqrestore_release(&process_lock,flags);return 1;}for(uint32_t d=0;d<LIONOS_PROCESS_MAX&&p->parent_pid;++d){p=find_pid(p->parent_pid);if(!p)break;if(p->pid==ancestor_pid){spinlock_irqrestore_release(&process_lock,flags);return 1;}}spinlock_irqrestore_release(&process_lock,flags);return 0;}
int32_t process_signal(uint32_t pid,uint32_t signal){uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*p=find_pid(pid);if(!p||p==&processes[0]||signal==0||signal>LIONOS_SIG_MAX){spinlock_irqrestore_release(&process_lock,flags);return LIONOS_SIGNAL_ERR;}if(signal==LIONOS_SIG_KILL||signal==LIONOS_SIG_TERM){p->pending_signals|=(1u<<(signal-1u));p->exit_code=128u+signal;p->state=PROCESS_ZOMBIE;wake_waiting_parent(p);spinlock_irqrestore_release(&process_lock,flags);return LIONOS_SIGNAL_OK;}if(signal==LIONOS_SIG_STOP){p->pending_signals|=(1u<<(signal-1u));p->state=PROCESS_STOPPED;spinlock_irqrestore_release(&process_lock,flags);return LIONOS_SIGNAL_OK;}if(signal==LIONOS_SIG_CONT){p->pending_signals&=~(1u<<(LIONOS_SIG_STOP-1u));if(p->state==PROCESS_STOPPED)p->state=PROCESS_READY;spinlock_irqrestore_release(&process_lock,flags);return LIONOS_SIGNAL_OK;}spinlock_irqrestore_release(&process_lock,flags);return LIONOS_SIGNAL_ERR;}
uint32_t process_signal_pending(uint32_t pid){uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*p=find_pid(pid);uint32_t r=p?p->pending_signals:0u;spinlock_irqrestore_release(&process_lock,flags);return r;}
int32_t process_get_state(uint32_t pid){uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*p=find_pid(pid);int32_t r=p?(int32_t)p->state:-1;spinlock_irqrestore_release(&process_lock,flags);return r;}
int process_set_current(struct process*p){uint32_t flags=spinlock_irqsave_acquire(&process_lock);if(!p||p->state==PROCESS_UNUSED||p->state==PROCESS_STOPPED||!p->page_directory){spinlock_irqrestore_release(&process_lock,flags);return -1;}struct process*old=current_local();if(old&&old!=p&&old->state==PROCESS_RUNNING)old->state=PROCESS_READY;set_current_local(p);p->state=PROCESS_RUNNING;paging_switch_address_space(p->page_directory);tss_set_kernel_stack(p->kernel_stack_top);spinlock_irqrestore_release(&process_lock,flags);return 0;}
uint32_t process_count(void){uint32_t flags=spinlock_irqsave_acquire(&process_lock),n=0;for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i)if(processes[i].state!=PROCESS_UNUSED)++n;spinlock_irqrestore_release(&process_lock,flags);return n;}
void process_set_saved_frame(struct process*p,uint32_t*f){uint32_t flags=spinlock_irqsave_acquire(&process_lock);if(p)p->saved_frame=(uint32_t)(uintptr_t)f;spinlock_irqrestore_release(&process_lock,flags);}
uint32_t*process_saved_frame(struct process*p){return p?(uint32_t *)(uintptr_t)p->saved_frame:0;}
uint32_t process_kernel_stack_top(struct process*p){return p?p->kernel_stack_top:0;}

uint32_t *process_schedule(uint32_t *frame){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);uint32_t cpu=cpu_current_index();struct process*prev=current_local();
    if(prev&&prev!=&processes[0])prev->saved_frame=(uint32_t)(uintptr_t)frame;
    if(prev&&prev->deferred_kernel_stack){page_free((void *)(uintptr_t)(prev->deferred_kernel_stack-4096u));prev->deferred_kernel_stack=0;}
    uint32_t start=prev?process_index(prev):0;
    for(uint32_t step=1;step<=LIONOS_PROCESS_MAX;++step){uint32_t idx=(start+step)%LIONOS_PROCESS_MAX;struct process*c=&processes[idx];if(c->state!=PROCESS_READY)continue;
        if(prev&&prev->state==PROCESS_RUNNING)prev->state=PROCESS_READY;set_current_local(c);c->state=PROCESS_RUNNING;paging_switch_address_space(c->page_directory);tss_set_kernel_stack(c->kernel_stack_top);
        if(prev&&prev->reap_pending)reap_process(prev);uint32_t*n=process_saved_frame(c);spinlock_irqrestore_release(&process_lock,flags);return n;
    }
    if(prev&&(prev->state==PROCESS_ZOMBIE||prev->state==PROCESS_STOPPED||prev->state==PROCESS_WAITING)){
        if(cpu==0){struct process*b=&processes[0];set_current_local(b);b->state=PROCESS_RUNNING;paging_switch_address_space(b->page_directory);tss_set_kernel_stack(b->kernel_stack_top);if(prev->reap_pending)reap_process(prev);uint32_t*n=process_saved_frame(b);spinlock_irqrestore_release(&process_lock,flags);return n;}
        set_current_local(0);if(prev->reap_pending)reap_process(prev);spinlock_irqrestore_release(&process_lock,flags);return frame;
    }
    if(!prev&&cpu==0){set_current_local(&processes[0]);processes[0].state=PROCESS_RUNNING;paging_switch_address_space(processes[0].page_directory);tss_set_kernel_stack(processes[0].kernel_stack_top);}
    spinlock_irqrestore_release(&process_lock,flags);return frame;
}
void scheduler_idle(void){for(;;)__asm__ volatile("sti; hlt");}
