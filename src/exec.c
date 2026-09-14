#include <stdint.h>
#include "elf.h"
#include "exec.h"
#include "memory.h"
#include "paging.h"
#include "process.h"
#include "ramfs.h"

#define USER_BASE 0x00400000u
#define USER_LIMIT 0xC0000000u
#define USER_STACK_PAGE 0xBFFFF000u
#define USER_STACK_TOP 0xC0000000u
#define PAGE_SIZE 4096u
#define MAX_EXEC_PAGES LIONOS_PROCESS_MAX_USER_PAGES

static uint32_t align_down(uint32_t x){return x&~(PAGE_SIZE-1u);}
static uint32_t align_up(uint32_t x){return (x+PAGE_SIZE-1u)&~(PAGE_SIZE-1u);}

int exec_validate_image(const uint8_t *image,uint32_t size,uint32_t *entry,uint32_t *stack_top){
    if(elf32_validate(image,size,entry)!=0)return -1;const struct elf32_header *h=(const struct elf32_header*)image;uint32_t load_count=0;
    for(uint32_t i=0;i<h->phnum;++i){const struct elf32_phdr *p=(const struct elf32_phdr*)(image+h->phoff+i*h->phentsize);if(p->type!=PT_LOAD||p->memsz==0)continue;++load_count;
        if(p->vaddr<USER_BASE||p->vaddr>=USER_LIMIT||p->memsz>USER_LIMIT-p->vaddr)return -1;if(p->filesz>p->memsz||p->offset>0xFFFFFFFFu-p->filesz)return -1;
        uint32_t end=p->vaddr+p->memsz,first=align_down(p->vaddr),last=align_up(end);if(end<p->vaddr||last<first||(last-first)/PAGE_SIZE>MAX_EXEC_PAGES)return -1;if(first<=USER_STACK_PAGE&&USER_STACK_PAGE<last)return -1;if((p->flags&7u)==0)return -1;
    }
    if(!load_count||*entry<USER_BASE||*entry>=USER_LIMIT)return -1;*stack_top=USER_STACK_TOP;return 0;
}

static int find_page(const uint32_t *vas,uint32_t count,uint32_t va){for(uint32_t i=0;i<count;++i)if(vas[i]==va)return (int)i;return -1;}
static int build_initial_stack(uint8_t *page,const char *name){
    uint32_t len=0;while(name[len]&&len<32u)++len;if(name[len]!=0||len+1u>64u)return -1;
    uint32_t string_va=USER_STACK_TOP-len-1u,argv0_va=USER_STACK_TOP-12u,argc_va=USER_STACK_TOP-20u;uint32_t os=string_va-USER_STACK_PAGE,oa=argv0_va-USER_STACK_PAGE,oc=argc_va-USER_STACK_PAGE;
    for(uint32_t i=0;i<=len;++i)page[os+i]=(uint8_t)(i<len?name[i]:0);*(uint32_t*)(page+oa)=string_va;*(uint32_t*)(page+oa+4u)=0;*(uint32_t*)(page+oc)=1u;return (int)argc_va;
}

static int load_image(const char *name,uint32_t *entry,uint32_t *stack,uint32_t *pd_out,uint32_t *pages,uint32_t *vas,uint32_t *count_out){
    const uint8_t *image=(const uint8_t*)ramfs_data(name);uint32_t size=ramfs_size(name),stack_top;if(!image||!size||exec_validate_image(image,size,entry,&stack_top)!=0)return -1;
    const struct elf32_header *h=(const struct elf32_header*)image;uint32_t flags[MAX_EXEC_PAGES],count=0;
    for(uint32_t i=0;i<h->phnum;++i){const struct elf32_phdr *p=(const struct elf32_phdr*)(image+h->phoff+i*h->phentsize);if(p->type!=PT_LOAD||!p->memsz)continue;uint32_t first=align_down(p->vaddr),last=align_up(p->vaddr+p->memsz);
        for(uint32_t va=first;va<last;va+=PAGE_SIZE){int idx=find_page(vas,count,va);if(idx<0){if(count>=MAX_EXEC_PAGES)goto fail;idx=(int)count++;vas[idx]=va;pages[idx]=(uint32_t)(uintptr_t)page_alloc();if(!pages[idx])goto fail;flags[idx]=1u;for(uint32_t b=0;b<PAGE_SIZE;++b)((uint8_t*)(uintptr_t)pages[idx])[b]=0;}if(p->flags&2u)flags[idx]|=2u;}
    }
    for(uint32_t i=0;i<h->phnum;++i){const struct elf32_phdr *p=(const struct elf32_phdr*)(image+h->phoff+i*h->phentsize);if(p->type!=PT_LOAD||!p->filesz)continue;for(uint32_t n=0;n<p->filesz;++n){uint32_t va=p->vaddr+n;int idx=find_page(vas,count,align_down(va));if(idx<0)goto fail;((uint8_t*)(uintptr_t)pages[idx])[va&(PAGE_SIZE-1u)]=image[p->offset+n];}}
    if(count>=MAX_EXEC_PAGES)goto fail;uint32_t sp=(uint32_t)(uintptr_t)page_alloc();if(!sp)goto fail;vas[count]=USER_STACK_PAGE;pages[count]=sp;flags[count]=3u;++count;for(uint32_t b=0;b<PAGE_SIZE;++b)((uint8_t*)(uintptr_t)sp)[b]=0;int initial_sp=build_initial_stack((uint8_t*)(uintptr_t)sp,name);if(initial_sp<0)goto fail;
    uint32_t pd=paging_create_address_space();if(!pd)goto fail;for(uint32_t i=0;i<count;++i)if(paging_map_user_page_in(pd,vas[i],pages[i],flags[i])!=0){paging_destroy_address_space(pd);goto fail;}
    *stack=(uint32_t)initial_sp;*pd_out=pd;*count_out=count;return 0;
fail:for(uint32_t i=0;i<count;++i)if(pages[i])page_free((void*)(uintptr_t)pages[i]);return -1;
}

int exec_run_file(const char *name){
    uint32_t entry,stack,pd,pages[MAX_EXEC_PAGES]={0},vas[MAX_EXEC_PAGES]={0},count=0;if(load_image(name,&entry,&stack,&pd,pages,vas,&count)!=0)return -1;
    struct process *p=process_create_ex_vas(entry,stack,pd,pages,vas,count);if(!p){paging_destroy_address_space(pd);for(uint32_t i=0;i<count;++i)if(pages[i])page_free((void*)(uintptr_t)pages[i]);return -1;}return (int)p->pid;
}

int exec_replace_current(const char *name){
    uint32_t entry,stack,pd,pages[MAX_EXEC_PAGES]={0},vas[MAX_EXEC_PAGES]={0},count=0;if(load_image(name,&entry,&stack,&pd,pages,vas,&count)!=0)return -1;
    if(process_exec_replace_current(entry,stack,pd,pages,vas,count)!=0){paging_destroy_address_space(pd);for(uint32_t i=0;i<count;++i)if(pages[i])page_free((void*)(uintptr_t)pages[i]);return -1;}return (int)process_current_pid();
}
