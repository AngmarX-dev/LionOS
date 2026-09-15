#include <stdint.h>
#include "memory.h"
#include "spinlock.h"

#define PAGE_SIZE 4096u
#define MAX_PAGES 65536u
#define BITMAP_WORDS (MAX_PAGES / 32u)
#define RESERVED_PAGES 1024u

#define MULTIBOOT2_TAG_END 0
#define MULTIBOOT2_TAG_MODULE 3
#define MULTIBOOT2_TAG_MMAP 6
#define MULTIBOOT2_MEMORY_AVAILABLE 1

struct mb2_tag { uint32_t type; uint32_t size; };
struct mb2_module_tag { uint32_t type,size,mod_start,mod_end; char string[0]; };
struct mb2_mmap_tag { uint32_t type,size,entry_size,entry_version; };
struct mb2_mmap_entry { uint64_t addr,len; uint32_t type,reserved; };
extern uint8_t _kernel_start; extern uint8_t _kernel_end;
static uint32_t bitmap[BITMAP_WORDS]; static uint32_t total_pages; static uint32_t free_pages;
static struct spinlock memory_lock;
static void mark_used(uint32_t p){if(p<MAX_PAGES)bitmap[p>>5]|=1u<<(p&31u);}
static void mark_free(uint32_t p){if(p<MAX_PAGES)bitmap[p>>5]&=~(1u<<(p&31u));}
static int is_free(uint32_t p){return p<MAX_PAGES&&!(bitmap[p>>5]&(1u<<(p&31u)));}
static void reserve_range(uint64_t s,uint64_t e){if(e<=s)return;s&=~(uint64_t)(PAGE_SIZE-1u);e=(e+PAGE_SIZE-1u)&~(uint64_t)(PAGE_SIZE-1u);for(uint64_t a=s;a<e;a+=PAGE_SIZE){uint32_t p=(uint32_t)(a/PAGE_SIZE);if(p<MAX_PAGES&&is_free(p)){mark_used(p);if(free_pages)--free_pages;}}}
void memory_init(uint32_t mi){spinlock_init(&memory_lock);for(uint32_t i=0;i<BITMAP_WORDS;++i)bitmap[i]=0xFFFFFFFFu;total_pages=free_pages=0;uint8_t*info=(uint8_t*)(uintptr_t)mi;uint32_t ts=*(uint32_t*)(uintptr_t)mi;uint8_t*tags=info+8u,*end=info+ts;while(tags+sizeof(struct mb2_tag)<=end){struct mb2_tag*t=(struct mb2_tag*)tags;if(t->type==MULTIBOOT2_TAG_END)break;if(t->type==MULTIBOOT2_TAG_MMAP){struct mb2_mmap_tag*m=(struct mb2_mmap_tag*)t;uint8_t*p=tags+sizeof(struct mb2_mmap_tag),*me=tags+m->size;while(p+m->entry_size<=me){struct mb2_mmap_entry*e=(struct mb2_mmap_entry*)p;if(e->type==MULTIBOOT2_MEMORY_AVAILABLE){uint64_t s=(e->addr+PAGE_SIZE-1u)&~(uint64_t)(PAGE_SIZE-1u),f=(e->addr+e->len)&~(uint64_t)(PAGE_SIZE-1u);for(uint64_t a=s;a<f;a+=PAGE_SIZE){uint32_t pg=(uint32_t)(a/PAGE_SIZE);if(pg<MAX_PAGES&&is_free(pg)){mark_free(pg);++free_pages;if(pg+1u>total_pages)total_pages=pg+1u;}}}p+=m->entry_size;}}tags+=(t->size+7u)&~7u;}reserve_range(0,RESERVED_PAGES*PAGE_SIZE);reserve_range((uint32_t)(uintptr_t)&_kernel_start,(uint32_t)(uintptr_t)&_kernel_end);reserve_range(mi,(uint64_t)mi+ts);tags=info+8u;while(tags+sizeof(struct mb2_tag)<=end){struct mb2_tag*t=(struct mb2_tag*)tags;if(t->type==MULTIBOOT2_TAG_END)break;if(t->type==MULTIBOOT2_TAG_MODULE&&t->size>=16u){struct mb2_module_tag*m=(struct mb2_module_tag*)t;reserve_range(m->mod_start,m->mod_end);}tags+=(t->size+7u)&~7u;}}
void*page_alloc_contiguous(uint32_t count){if(!count||count>MAX_PAGES-RESERVED_PAGES)return 0;uint32_t irq=spinlock_irqsave_acquire(&memory_lock);uint32_t run=0,start=RESERVED_PAGES;for(uint32_t p=RESERVED_PAGES;p<MAX_PAGES;++p){if(is_free(p)){if(!run)start=p;if(++run==count){for(uint32_t i=0;i<count;++i)mark_used(start+i);free_pages-=count;spinlock_irqrestore_release(&memory_lock,irq);return(void*)(uintptr_t)(start*PAGE_SIZE);}}else run=0;}spinlock_irqrestore_release(&memory_lock,irq);return 0;}
void*page_alloc(void){return page_alloc_contiguous(1);}
void page_free(void*ptr){uint32_t a=(uint32_t)(uintptr_t)ptr;if((a&(PAGE_SIZE-1u))!=0)return;uint32_t p=a/PAGE_SIZE;if(p<RESERVED_PAGES||p>=MAX_PAGES)return;uint32_t irq=spinlock_irqsave_acquire(&memory_lock);if(!is_free(p)){mark_free(p);++free_pages;}spinlock_irqrestore_release(&memory_lock,irq);}
uint32_t memory_total_pages(void){return total_pages;}uint32_t memory_free_pages(void){uint32_t irq=spinlock_irqsave_acquire(&memory_lock);uint32_t n=free_pages;spinlock_irqrestore_release(&memory_lock,irq);return n;}
