#include <stdint.h>
#include "memory.h"
#include "phys_range.h"
#include "multiboot2.h"
#include "debug.h"
#include "spinlock.h"

#define PAGE_SIZE 4096u
#define MAX_PAGES 65536u
#define RESERVED_PAGES 2048u

struct mb2_mmap_entry { uint64_t addr,len; uint32_t type,reserved; };
extern uint8_t _kernel_start; extern uint8_t _kernel_end;
static uint32_t bitmap[MAX_PAGES / 32u]; static uint16_t page_refs[MAX_PAGES]; static uint32_t total_pages; static uint32_t free_pages; static uint64_t high_available_bytes;
static struct spinlock memory_lock;
static void mark_used(uint32_t p){if(p<MAX_PAGES)bitmap[p>>5]|=1u<<(p&31u);}
static void mark_free(uint32_t p){if(p<MAX_PAGES)bitmap[p>>5]&=~(1u<<(p&31u));}
static int is_free(uint32_t p){return p<MAX_PAGES&&!(bitmap[p>>5]&(1u<<(p&31u)));}
static void reserve_low_range(uint64_t start,uint64_t end)
{
    uint64_t first, last;
    int result = phys_range_page_bounds(start, end, 0ULL,
                                         (uint64_t)MAX_PAGES * PAGE_SIZE,
                                         1, &first, &last);
    if (result != 1)
        return;
    for (uint64_t address = first; address < last; address += PAGE_SIZE) {
        uint32_t page = (uint32_t)(address / PAGE_SIZE);
        if (is_free(page)) {
            mark_used(page);
            if (free_pages)
                --free_pages;
        }
    }
}

static void mark_low_available(uint64_t start,uint64_t end)
{
    uint64_t first, last;
    int result = phys_range_page_bounds(start, end, 0ULL,
                                         (uint64_t)MAX_PAGES * PAGE_SIZE,
                                         0, &first, &last);
    if (result != 1)
        return;
    for (uint64_t address = first; address < last; address += PAGE_SIZE) {
        uint32_t page = (uint32_t)(address / PAGE_SIZE);
        if (!is_free(page)) {
            mark_free(page);
            page_refs[page] = 0u;
            ++free_pages;
            if (page + 1u > total_pages)
                total_pages = page + 1u;
        }
    }
}
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
static void high_mark_available(uint64_t start,uint64_t end)
{
    uint64_t first, last;
    int result = phys_range_page_bounds(start, end, HIGH_MEMORY_BASE,
                                         HIGH_MEMORY_LIMIT, 0,
                                         &first, &last);
    if (result != 1)
        return;
    for (uint64_t address = first; address < last; address += PAGE_SIZE) {
        uint32_t page = (uint32_t)((address - HIGH_MEMORY_BASE) / PAGE_SIZE);
        if (!high_bit_get(high_available, page)) {
            high_bit_set(high_available, page);
            ++high_free_pages;
        }
    }
}

static void high_reserve_range(uint64_t start,uint64_t end)
{
    uint64_t first, last;
    int result = phys_range_page_bounds(start, end, HIGH_MEMORY_BASE,
                                         HIGH_MEMORY_LIMIT, 1,
                                         &first, &last);
    if (result != 1)
        return;
    for (uint64_t address = first; address < last; address += PAGE_SIZE) {
        uint32_t page = (uint32_t)((address - HIGH_MEMORY_BASE) / PAGE_SIZE);
        if (high_bit_get(high_available, page)) {
            high_bit_clear(high_available, page);
            if (high_free_pages)
                --high_free_pages;
        }
        high_bit_clear(high_allocated, page);
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

    /*
     * Validate the complete Multiboot2 tag stream before consuming any
     * memory-map or module records. A malformed boot handoff must leave every
     * allocator page reserved rather than exposing partially parsed RAM.
     */
    if(multiboot2_info_validate((const void *)(uintptr_t)mi)!=0){
        debug_write("LIONOS:MB2-INFO-INVALID-MEMORY-UNAVAILABLE\\n");
        return;
    }

    uint32_t ts=*(const uint32_t *)(uintptr_t)mi;
    mb2_info_iterator_t iterator;
    mb2_tag_view_t tag;
    if(multiboot2_info_iterator_init((const void *)(uintptr_t)mi,&iterator)!=0)
        return;

    int next = 0;
    /*
     * First publish only firmware-available pages. The second pass applies
     * every non-available descriptor, so reserved/MMIO ranges always win
     * over overlapping available descriptors regardless of map ordering.
     */
    for (uint32_t pass = 0u; pass < 2u; ++pass) {
        if (multiboot2_info_iterator_init((const void *)(uintptr_t)mi,
                                          &iterator) != 0) {
            next = -1;
            break;
        }
        while ((next = multiboot2_info_iterator_next(&iterator, &tag)) > 0) {
            if (tag.type != MULTIBOOT2_TAG_MMAP)
                continue;
            const uint8_t *entries = tag.payload + 8u;
            uint32_t entries_bytes = tag.size - 16u;
            uint32_t entry_size =
                *(const uint32_t *)(const void *)tag.payload;
            for (uint32_t offset = 0u; offset < entries_bytes;
                 offset += entry_size) {
                const struct mb2_mmap_entry *entry =
                    (const struct mb2_mmap_entry *)(const void *)(entries + offset);
                if (entry->len > UINT64_MAX - entry->addr)
                    continue;
                uint64_t region_end = entry->addr + entry->len;
                if (pass == 0u) {
                    if (entry->type != MULTIBOOT2_MEMORY_AVAILABLE)
                        continue;
                    mark_low_available(entry->addr, region_end);
                    high_mark_available(entry->addr, region_end);
                } else if (entry->type != MULTIBOOT2_MEMORY_AVAILABLE) {
                    reserve_low_range(entry->addr, region_end);
                    high_reserve_range(entry->addr, region_end);
                }
            }
        }
        if (next < 0)
            break;
        if (pass == 0u) {
            /* Count the unique firmware-available high pages, not raw sums. */
            high_available_bytes = (uint64_t)high_free_pages * PAGE_SIZE;
        }
    }
    if(next<0){
        for(uint32_t i=0u;i<MAX_PAGES/32u;++i)bitmap[i]=0xFFFFFFFFu;
        for(uint32_t i=0u;i<MAX_PAGES;++i)page_refs[i]=0u;
        for(uint32_t i=0u;i<HIGH_BITMAP_WORDS;++i){high_available[i]=0u;high_allocated[i]=0u;}
        total_pages=free_pages=high_free_pages=high_scan_hint=0u;
        high_available_bytes=0u;
        debug_write("LIONOS:MB2-INFO-ITERATION-FAILED-MEMORY-UNAVAILABLE\\n");
        return;
    }

    reserve_low_range(0,RESERVED_PAGES*PAGE_SIZE);
    reserve_low_range((uint32_t)(uintptr_t)&_kernel_start,(uint32_t)(uintptr_t)&_kernel_end);
    reserve_low_range(mi,(uint64_t)mi+ts);
    high_reserve_range(mi,(uint64_t)mi+ts);

    /* Reserve every module only after all available regions were discovered. */
    if(multiboot2_info_iterator_init((const void *)(uintptr_t)mi,&iterator)!=0)
        return;
    while((next=multiboot2_info_iterator_next(&iterator,&tag))>0){
        if(tag.type==MULTIBOOT2_TAG_MODULE){
            uint32_t module_start=*(const uint32_t *)(const void *)tag.payload;
            uint32_t module_end=*(const uint32_t *)(const void *)(tag.payload+4u);
            reserve_low_range(module_start,module_end);
            high_reserve_range(module_start,module_end);
        }
    }
    if(next<0){
        debug_write("LIONOS:MB2-MODULE-ITERATION-FAILED\\n");
        return;
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

void memory_reserve_physical_range(uint64_t start,uint64_t end){
    if(end<=start)return;
    uint32_t irq=spinlock_irqsave_acquire(&memory_lock);
    high_reserve_range(start,end);
    reserve_low_range(start, end);
    spinlock_irqrestore_release(&memory_lock,irq);
}

void page_free_high(uint64_t physical){
    if((physical&(PAGE_SIZE-1u))||physical<HIGH_MEMORY_BASE||physical>=HIGH_MEMORY_LIMIT)return;
    uint32_t page=(uint32_t)((physical-HIGH_MEMORY_BASE)/PAGE_SIZE);
    uint32_t irq=spinlock_irqsave_acquire(&memory_lock);
    if(high_bit_get(high_allocated,page)){
        high_bit_clear(high_allocated,page);
        if(high_bit_get(high_available,page)){
            ++high_free_pages;
            if(page<high_scan_hint)high_scan_hint=page;
        }
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

int memory_range_contains_available_ram(uint64_t start, uint64_t end){
    if(end<=start || (start&(PAGE_SIZE-1u)) || (end&(PAGE_SIZE-1u)) ||
       start<HIGH_MEMORY_BASE || end>HIGH_MEMORY_LIMIT)
        return -1;
    uint32_t irq=spinlock_irqsave_acquire(&memory_lock);
    int contains_ram=0;
    for(uint64_t address=start;address<end;address+=PAGE_SIZE){
        uint32_t page=(uint32_t)((address-HIGH_MEMORY_BASE)/PAGE_SIZE);
        if(high_bit_get(high_available,page)){
            contains_ram=1;
            break;
        }
    }
    spinlock_irqrestore_release(&memory_lock,irq);
    return contains_ram;
}

uint64_t memory_high_available_bytes(void){return high_available_bytes;}
