#include <stdint.h>
#include "memory.h"
#include "spinlock.h"

#define PAGE_SIZE 4096u
#define MAX_PAGES 65536u
#define RESERVED_PAGES 2048u

#define MULTIBOOT2_TAG_END 0
#define MULTIBOOT2_TAG_MODULE 3
#define MULTIBOOT2_TAG_MMAP 6
#define MULTIBOOT2_MEMORY_AVAILABLE 1

struct mb2_tag { uint32_t type; uint32_t size; };
struct mb2_module_tag { uint32_t type,size,mod_start,mod_end; char string[0]; };
struct mb2_mmap_tag { uint32_t type,size,entry_size,entry_version; };
struct mb2_mmap_entry { uint64_t addr,len; uint32_t type,reserved; };
extern uint8_t _kernel_start; extern uint8_t _kernel_end;
static uint32_t bitmap[MAX_PAGES / 32u]; static uint16_t page_refs[MAX_PAGES]; static uint32_t total_pages; static uint32_t free_pages; static uint64_t high_available_bytes;
static struct spinlock memory_lock;
static void mark_used(uint32_t p){if(p<MAX_PAGES)bitmap[p>>5]|=1u<<(p&31u);}
static void mark_free(uint32_t p){if(p<MAX_PAGES)bitmap[p>>5]&=~(1u<<(p&31u));}
static int is_free(uint32_t p){return p<MAX_PAGES&&!(bitmap[p>>5]&(1u<<(p&31u)));}
static void reserve_range(uint64_t s,uint64_t e){if(e<=s)return;s&=~(uint64_t)(PAGE_SIZE-1u);e=(e+PAGE_SIZE-1u)&~(uint64_t)(PAGE_SIZE-1u);for(uint64_t a=s;a<e;a+=PAGE_SIZE){uint32_t p=(uint32_t)(a/PAGE_SIZE);if(p<MAX_PAGES&&is_free(p)){mark_used(p);if(free_pages)--free_pages;}}}
#define HIGH_MEMORY_BASE 0x10000000ULL
#define HIGH_MEMORY_LIMIT 0x1000000000ULL
#define HIGH_MEMORY_PAGES ((uint32_t)((HIGH_MEMORY_LIMIT-HIGH_MEMORY_BASE)/PAGE_SIZE))
#define HIGH_BITMAP_WORDS ((HIGH_MEMORY_PAGES+31u)/32u)
static uint32_t high_available[HIGH_BITMAP_WORDS];
static uint32_t high_allocated[HIGH_BITMAP_WORDS];
static uint32_t high_scan_hint;
static uint32_t high_free_pages;

static int high_bit_get(const uint32_t *map,uint32_t page){return (map[page>>5]&(1u<<(page&31u)))!=0u;}
static void high_bit_set(uint32_t *map,uint32_t page){map[page>>5]|=1u<<(page&31u);}
static void high_bit_clear(uint32_t *map,uint32_t page){map[page>>5]&=~(1u<<(page&31u));}
static void high_mark_available(uint64_t start,uint64_t end){
    if(end<=start||end<=HIGH_MEMORY_BASE||start>=HIGH_MEMORY_LIMIT)return;
    if(start<HIGH_MEMORY_BASE)start=HIGH_MEMORY_BASE;
    if(end>HIGH_MEMORY_LIMIT)end=HIGH_MEMORY_LIMIT;
    start=(start+PAGE_SIZE-1u)&~(uint64_t)(PAGE_SIZE-1u);
    end&=~(uint64_t)(PAGE_SIZE-1u);
    for(uint64_t a=start;a<end;a+=PAGE_SIZE){
        uint32_t page=(uint32_t)((a-HIGH_MEMORY_BASE)/PAGE_SIZE);
        if(!high_bit_get(high_available,page)){high_bit_set(high_available,page);++high_free_pages;}
    }
}
static void high_reserve_range(uint64_t start,uint64_t end){
    if(end<=start||end<=HIGH_MEMORY_BASE||start>=HIGH_MEMORY_LIMIT)return;
    if(start<HIGH_MEMORY_BASE)start=HIGH_MEMORY_BASE;
    if(end>HIGH_MEMORY_LIMIT)end=HIGH_MEMORY_LIMIT;
    start&=~(uint64_t)(PAGE_SIZE-1u);
    end=(end+PAGE_SIZE-1u)&~(uint64_t)(PAGE_SIZE-1u);
    if(end>HIGH_MEMORY_LIMIT)end=HIGH_MEMORY_LIMIT;
    for(uint64_t a=start;a<end;a+=PAGE_SIZE){
        uint32_t page=(uint32_t)((a-HIGH_MEMORY_BASE)/PAGE_SIZE);
        if(high_bit_get(high_available,page)){high_bit_clear(high_available,page);if(high_free_pages)--high_free_pages;}
    }
}

void memory_init(uint32_t mi){
    spinlock_init(&memory_lock);
    for(uint32_t i=0;i<MAX_PAGES/32u;++i)bitmap[i]=0xFFFFFFFFu;
    for(uint32_t i=0;i<MAX_PAGES;++i)page_refs[i]=0u;
    for(uint32_t i=0;i<HIGH_BITMAP_WORDS;++i){high_available[i]=0u;high_allocated[i]=0u;}
    total_pages=free_pages=0u;
    high_free_pages=0u;
    high_scan_hint=0u;
    high_available_bytes=0u;
    uint8_t*info=(uint8_t*)(uintptr_t)mi;
    uint32_t ts=*(uint32_t*)(uintptr_t)mi;
    uint8_t*tags=info+8u,*end=info+ts;
    while(tags+sizeof(struct mb2_tag)<=end){
        struct mb2_tag*t=(struct mb2_tag*)tags;
        if(t->type==MULTIBOOT2_TAG_END)break;
        if(t->type==MULTIBOOT2_TAG_MMAP){
            struct mb2_mmap_tag*m=(struct mb2_mmap_tag*)t;
            uint8_t*p=tags+sizeof(struct mb2_mmap_tag),*me=tags+m->size;
            while(p+m->entry_size<=me){
                struct mb2_mmap_entry*e=(struct mb2_mmap_entry*)p;
                uint64_t region_end=e->addr+e->len;
                if(region_end>=e->addr){
                    if(e->type==MULTIBOOT2_MEMORY_AVAILABLE){
                        if(region_end>0x10000000ULL){
                            uint64_t hs=e->addr>0x10000000ULL?e->addr:0x10000000ULL;
                            hs=(hs+PAGE_SIZE-1u)&~(uint64_t)(PAGE_SIZE-1u);
                            uint64_t he=region_end&~(uint64_t)(PAGE_SIZE-1u);
                            if(he>hs&&UINT64_MAX-high_available_bytes>=he-hs)high_available_bytes+=he-hs;
                        }
                        high_mark_available(e->addr,region_end);
                        uint64_t s=(e->addr+PAGE_SIZE-1u)&~(uint64_t)(PAGE_SIZE-1u);
                        uint64_t f=region_end&~(uint64_t)(PAGE_SIZE-1u);
                        for(uint64_t a=s;a<f;a+=PAGE_SIZE){
                            uint32_t pg=(uint32_t)(a/PAGE_SIZE);
                            if(pg<MAX_PAGES&&!is_free(pg)){
                                mark_free(pg);page_refs[pg]=0u;++free_pages;
                                if(pg+1u>total_pages)total_pages=pg+1u;
                            }
                        }
                    }else{
                        high_reserve_range(e->addr,region_end);
                    }
                }
                p+=m->entry_size;
            }
        }
        tags+=(t->size+7u)&~7u;
    }
    reserve_range(0,RESERVED_PAGES*PAGE_SIZE);
    reserve_range((uint32_t)(uintptr_t)&_kernel_start,(uint32_t)(uintptr_t)&_kernel_end);
    reserve_range(mi,(uint64_t)mi+ts);
    high_reserve_range(mi,(uint64_t)mi+ts);
    tags=info+8u;
    while(tags+sizeof(struct mb2_tag)<=end){
        struct mb2_tag*t=(struct mb2_tag*)tags;
        if(t->type==MULTIBOOT2_TAG_END)break;
        if(t->type==MULTIBOOT2_TAG_MODULE&&t->size>=16u){
            struct mb2_module_tag*m=(struct mb2_module_tag*)t;
            reserve_range(m->mod_start,m->mod_end);
            high_reserve_range(m->mod_start,m->mod_end);
        }
        tags+=(t->size+7u)&~7u;
    }
}

int page_alloc_high(uint64_t *physical_address){
    if(!physical_address)return -1;
    uint32_t irq=spinlock_irqsave_acquire(&memory_lock);
    if(!high_free_pages){spinlock_irqrestore_release(&memory_lock,irq);return -1;}
    for(uint32_t pass=0u;pass<2u;++pass){
        uint32_t begin=pass?0u:high_scan_hint;
        uint32_t finish=pass?high_scan_hint:HIGH_MEMORY_PAGES;
        for(uint32_t page=begin;page<finish;++page){
            if(high_bit_get(high_available,page)&&!high_bit_get(high_allocated,page)){
                high_bit_set(high_allocated,page);
                --high_free_pages;
                high_scan_hint=page+1u;
                if(high_scan_hint>=HIGH_MEMORY_PAGES)high_scan_hint=0u;
                *physical_address=HIGH_MEMORY_BASE+(uint64_t)page*PAGE_SIZE;
                spinlock_irqrestore_release(&memory_lock,irq);
                return 0;
            }
        }
    }
    spinlock_irqrestore_release(&memory_lock,irq);
    return -1;
}

void page_free_high(uint64_t physical){
    if((physical&(PAGE_SIZE-1u))||physical<HIGH_MEMORY_BASE||physical>=HIGH_MEMORY_LIMIT)return;
    uint32_t page=(uint32_t)((physical-HIGH_MEMORY_BASE)/PAGE_SIZE);
    uint32_t irq=spinlock_irqsave_acquire(&memory_lock);
    if(high_bit_get(high_allocated,page)){
        high_bit_clear(high_allocated,page);
        ++high_free_pages;
        if(page<high_scan_hint)high_scan_hint=page;
    }
    spinlock_irqrestore_release(&memory_lock,irq);
}

uint32_t memory_high_free_pages(void){
    uint32_t irq=spinlock_irqsave_acquire(&memory_lock);
    uint32_t count=high_free_pages;
    spinlock_irqrestore_release(&memory_lock,irq);
    return count;
}
void*page_alloc_contiguous(uint32_t count){if(!count||count>MAX_PAGES-RESERVED_PAGES)return 0;uint32_t irq=spinlock_irqsave_acquire(&memory_lock);uint32_t run=0,start=RESERVED_PAGES;for(uint32_t p=RESERVED_PAGES;p<MAX_PAGES;++p){if(is_free(p)){if(!run)start=p;if(++run==count){for(uint32_t i=0;i<count;++i){mark_used(start+i);page_refs[start+i]=1u;}free_pages-=count;spinlock_irqrestore_release(&memory_lock,irq);return(void*)(uintptr_t)(start*PAGE_SIZE);}}else run=0;}spinlock_irqrestore_release(&memory_lock,irq);return 0;}
void*page_alloc(void){return page_alloc_contiguous(1);}
void page_retain(void*ptr){uint32_t a=(uint32_t)(uintptr_t)ptr;if((a&(PAGE_SIZE-1u))!=0)return;uint32_t p=a/PAGE_SIZE;if(p<RESERVED_PAGES||p>=MAX_PAGES)return;uint32_t irq=spinlock_irqsave_acquire(&memory_lock);if(!is_free(p)&&page_refs[p]<0xFFFFu)++page_refs[p];spinlock_irqrestore_release(&memory_lock,irq);}
uint32_t page_refcount(void*ptr){uint32_t a=(uint32_t)(uintptr_t)ptr;if((a&(PAGE_SIZE-1u))!=0)return 0u;uint32_t p=a/PAGE_SIZE;if(p<RESERVED_PAGES||p>=MAX_PAGES)return 0u;uint32_t irq=spinlock_irqsave_acquire(&memory_lock);uint32_t n=is_free(p)?0u:(uint32_t)page_refs[p];spinlock_irqrestore_release(&memory_lock,irq);return n;}
void page_free(void*ptr){uint32_t a=(uint32_t)(uintptr_t)ptr;if((a&(PAGE_SIZE-1u))!=0)return;uint32_t p=a/PAGE_SIZE;if(p<RESERVED_PAGES||p>=MAX_PAGES)return;uint32_t irq=spinlock_irqsave_acquire(&memory_lock);if(!is_free(p)&&page_refs[p]){if(--page_refs[p]==0u){mark_free(p);++free_pages;}}spinlock_irqrestore_release(&memory_lock,irq);}
uint32_t memory_total_pages(void){return total_pages;}uint32_t memory_free_pages(void){uint32_t irq=spinlock_irqsave_acquire(&memory_lock);uint32_t n=free_pages;spinlock_irqrestore_release(&memory_lock,irq);return n;}uint64_t memory_managed_limit_bytes(void){return (uint64_t)MAX_PAGES*PAGE_SIZE;}

uint64_t memory_high_available_bytes(void){return high_available_bytes;}
