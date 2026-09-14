#include <stdint.h>
#include "memory.h"
#include "paging.h"
#include "process.h"
#include "tss.h"

static struct process processes[LIONOS_PROCESS_MAX];
static struct process *current;
static uint32_t next_pid;
static struct process *find_free_slot(void) { for (uint32_t i=0;i<LIONOS_PROCESS_MAX;++i) if (processes[i].state==PROCESS_UNUSED) return &processes[i]; return 0; }
static uint32_t process_index(const struct process *p) { return (uint32_t)(p-processes); }
static void init_interrupt_frame(struct process *p) {
    uint32_t *f=(uint32_t *)(uintptr_t)(p->kernel_stack_top-PROCESS_CONTEXT_WORDS*4u);
    for(uint32_t i=0;i<PROCESS_CONTEXT_WORDS;++i) f[i]=0;
    f[0]=0x2Bu;f[1]=0x2Bu;f[2]=0x2Bu;f[3]=0x2Bu;f[14]=p->entry;f[15]=0x23u;f[16]=0x202u;f[17]=p->user_stack;f[18]=0x2Bu;p->saved_frame=(uint32_t)(uintptr_t)f;
}
static void init_bootstrap_frame(void) {
    struct process *b=&processes[0]; void *s=page_alloc(); if(!s){b->kernel_stack_top=0;b->saved_frame=0;return;}
    b->kernel_stack_top=(uint32_t)(uintptr_t)s+4096u;b->page_directory=paging_kernel_directory();b->entry=(uint32_t)(uintptr_t)&process_schedule;b->user_stack=b->kernel_stack_top;
    uint32_t *f=(uint32_t *)(uintptr_t)(b->kernel_stack_top-PROCESS_CONTEXT_WORDS*4u);for(uint32_t i=0;i<PROCESS_CONTEXT_WORDS;++i)f[i]=0;
    f[0]=0x10u;f[1]=0x10u;f[2]=0x10u;f[3]=0x10u;f[14]=(uint32_t)(uintptr_t)&scheduler_idle;f[15]=0x08u;f[16]=0x202u;f[17]=b->kernel_stack_top;f[18]=0x10u;b->saved_frame=(uint32_t)(uintptr_t)f;
}
void process_init(void){
    for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i){processes[i].pid=0;processes[i].state=PROCESS_UNUSED;processes[i].page_directory=0;processes[i].entry=0;processes[i].user_stack=0;processes[i].kernel_stack_top=0;processes[i].saved_frame=0;processes[i].user_code_page=0;processes[i].user_stack_page=0;processes[i].user_page_count=0;for(uint32_t j=0;j<LIONOS_PROCESS_MAX_USER_PAGES;++j){processes[i].user_pages[j]=0;processes[i].user_page_vas[j]=0;}}
    next_pid=2;processes[0].pid=1;processes[0].state=PROCESS_RUNNING;current=&processes[0];init_bootstrap_frame();
}
struct process *process_current(void){return current;} struct process *process_at(uint32_t i){return i<LIONOS_PROCESS_MAX?&processes[i]:0;} uint32_t process_current_pid(void){return current?current->pid:0;}
const char *process_state_name(uint32_t s){switch(s){case PROCESS_READY:return "READY";case PROCESS_RUNNING:return "RUNNING";case PROCESS_ZOMBIE:return "ZOMBIE";default:return "UNUSED";}}
struct process *process_create_ex_vas(uint32_t entry,uint32_t stack,uint32_t pd,const uint32_t *pages,const uint32_t *vas,uint32_t count){
    struct process *p=find_free_slot();if(!p||!pd||!pages||!vas||!count||count>LIONOS_PROCESS_MAX_USER_PAGES)return 0;void *ks=page_alloc();if(!ks)return 0;
    p->pid=next_pid++;if(next_pid==0)next_pid=2;p->state=PROCESS_READY;p->page_directory=pd;p->entry=entry;p->user_stack=stack;p->kernel_stack_top=(uint32_t)(uintptr_t)ks+4096u;p->saved_frame=0;p->user_page_count=count;p->user_code_page=pages[0];p->user_stack_page=pages[count-1u];
    for(uint32_t i=0;i<count;++i){p->user_pages[i]=pages[i];p->user_page_vas[i]=vas[i];}for(uint32_t i=count;i<LIONOS_PROCESS_MAX_USER_PAGES;++i){p->user_pages[i]=0;p->user_page_vas[i]=0;}init_interrupt_frame(p);return p;
}
struct process *process_create_ex(uint32_t entry,uint32_t stack,uint32_t pd,const uint32_t *pages,uint32_t count){uint32_t vas[LIONOS_PROCESS_MAX_USER_PAGES];for(uint32_t i=0;i<count&&i<LIONOS_PROCESS_MAX_USER_PAGES;++i)vas[i]=i*4096u;return process_create_ex_vas(entry,stack,pd,pages,vas,count);}
struct process *process_create(uint32_t entry,uint32_t stack,uint32_t pd,uint32_t code,uint32_t user_stack_page){uint32_t p[2]={code,user_stack_page},v[2]={0x00400000u,0x00401000u};return process_create_ex_vas(entry,stack,pd,p,v,2u);}
uint32_t process_fork_current(uint32_t *parent_frame){
    if(!current||current==&processes[0]||!parent_frame||!current->user_page_count)return 0;struct process *slot=find_free_slot();if(!slot)return 0;uint32_t pd=paging_create_address_space();if(!pd)return 0;
    uint32_t copied[LIONOS_PROCESS_MAX_USER_PAGES]={0};uint32_t count=current->user_page_count;
    for(uint32_t i=0;i<count;++i){uint32_t src,flags;if(paging_get_user_page(current->page_directory,current->user_page_vas[i],&src,&flags)!=0)goto fail;void *dst=page_alloc();if(!dst)goto fail;copied[i]=(uint32_t)(uintptr_t)dst;for(uint32_t b=0;b<4096u;++b)((uint8_t*)dst)[b]=((const uint8_t*)(uintptr_t)src)[b];if(paging_map_user_page_in(pd,current->user_page_vas[i],copied[i],flags)!=0)goto fail;}
    slot=process_create_ex_vas(current->entry,current->user_stack,pd,copied,current->user_page_vas,count);if(!slot)goto fail;{uint32_t *cf=process_saved_frame(slot);for(uint32_t i=0;i<PROCESS_CONTEXT_WORDS;++i)cf[i]=parent_frame[i];cf[11]=0;}return slot->pid;
fail:for(uint32_t i=0;i<count;++i)if(copied[i])page_free((void *)(uintptr_t)copied[i]);paging_destroy_address_space(pd);return 0;
}
int process_set_current(struct process *p){if(!p||p->state==PROCESS_UNUSED||!p->page_directory)return -1;if(current&&current!=p&&current->state==PROCESS_RUNNING)current->state=PROCESS_READY;current=p;current->state=PROCESS_RUNNING;paging_switch_address_space(p->page_directory);tss_set_kernel_stack(p->kernel_stack_top);return 0;}
void process_exit_current(void){if(current&&current!=&processes[0])current->state=PROCESS_ZOMBIE;}
static void reap_process(struct process *p){if(!p||p->state!=PROCESS_ZOMBIE)return;for(uint32_t i=0;i<p->user_page_count;++i)if(p->user_pages[i])page_free((void *)(uintptr_t)p->user_pages[i]);if(p->page_directory)paging_destroy_address_space(p->page_directory);if(p->kernel_stack_top)page_free((void *)(uintptr_t)(p->kernel_stack_top-4096u));p->pid=0;p->state=PROCESS_UNUSED;p->page_directory=0;p->entry=0;p->user_stack=0;p->kernel_stack_top=0;p->saved_frame=0;p->user_code_page=0;p->user_stack_page=0;p->user_page_count=0;for(uint32_t i=0;i<LIONOS_PROCESS_MAX_USER_PAGES;++i){p->user_pages[i]=0;p->user_page_vas[i]=0;}}
uint32_t process_count(void){uint32_t n=0;for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i)if(processes[i].state!=PROCESS_UNUSED)++n;return n;}
void process_set_saved_frame(struct process *p,uint32_t *f){if(p)p->saved_frame=(uint32_t)(uintptr_t)f;}uint32_t *process_saved_frame(struct process *p){return p?(uint32_t *)(uintptr_t)p->saved_frame:0;}uint32_t process_kernel_stack_top(struct process *p){return p?p->kernel_stack_top:0;}
uint32_t *process_schedule(uint32_t *frame){if(!current)return frame;struct process *prev=current;if(current->state==PROCESS_RUNNING)current->saved_frame=(uint32_t)(uintptr_t)frame;uint32_t start=process_index(current);for(uint32_t step=1;step<=LIONOS_PROCESS_MAX;++step){uint32_t idx=(start+step)%LIONOS_PROCESS_MAX;struct process *c=&processes[idx];if(c->state!=PROCESS_READY)continue;if(current->state==PROCESS_RUNNING)current->state=PROCESS_READY;current=c;current->state=PROCESS_RUNNING;paging_switch_address_space(current->page_directory);tss_set_kernel_stack(current->kernel_stack_top);if(prev!=current)reap_process(prev);return process_saved_frame(current);}if(current->state==PROCESS_ZOMBIE){current=&processes[0];current->state=PROCESS_RUNNING;paging_switch_address_space(current->page_directory);tss_set_kernel_stack(current->kernel_stack_top);reap_process(prev);return process_saved_frame(current);}current->state=PROCESS_RUNNING;tss_set_kernel_stack(current->kernel_stack_top);return frame;}
void scheduler_idle(void){for(;;)__asm__ volatile("sti; hlt");}
