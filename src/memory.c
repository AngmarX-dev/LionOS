#include <stdint.h>
#include "memory.h"
#include "phys_range.h"
#include "multiboot2.h"
#include "debug.h"
#include "spinlock.h"

#define PAGE_SIZE 4096u
#define MAX_PAGES 65536u
#define RESERVED_PAGES 2048u

extern uint8_t _kernel_start; extern uint8_t _kernel_end;

/* Decode untyped Multiboot2 byte streams without integer-pointer aliasing. */
static uint32_t mb2_read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static uint64_t mb2_read_u64(const uint8_t *bytes)
{
    return (uint64_t)mb2_read_u32(bytes) |
           ((uint64_t)mb2_read_u32(bytes + 4u) << 32);
}
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
        /*
         * A runtime reservation revokes any outstanding allocation too.
         * Keep the page permanently unavailable and invalidate stale refs.
         */
        page_refs[page] = 0u;
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

/* Sparse metadata is bounded by the 8192-slot temporary high-map window. */
#define HIGH_PAGE_PIN_SLOTS 8192u
struct high_page_pin_record {
    uint32_t page;
    uint16_t references;
    uint8_t used;
    uint8_t pending_free;
};
static struct high_page_pin_record high_page_pins[HIGH_PAGE_PIN_SLOTS];

static int high_pin_find(uint32_t page)
{
    for (uint32_t i = 0u; i < HIGH_PAGE_PIN_SLOTS; ++i) {
        if (high_page_pins[i].used && high_page_pins[i].page == page)
            return (int)i;
    }
    return -1;
}

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
            /* Allocated pages are already excluded from the free count. */
            if (!high_bit_get(high_allocated, page) && high_free_pages)
                --high_free_pages;
            high_bit_clear(high_available, page);
        }

        /*
         * Only allocated pages can have pin records. Avoid scanning the
         * bounded pin table for unallocated pages in large reservation ranges.
         */
        if (high_bit_get(high_allocated, page)) {
            int pin_slot = high_pin_find(page);
            if (pin_slot >= 0)
                high_page_pins[(uint32_t)pin_slot].pending_free = 1u;
            else
                high_bit_clear(high_allocated, page);
        }
    }
}

static void memory_reset_unavailable(void)
{
    for (uint32_t i = 0u; i < MAX_PAGES / 32u; ++i) bitmap[i] = 0xFFFFFFFFu;
    for (uint32_t i = 0u; i < MAX_PAGES; ++i) page_refs[i] = 0u;
    for (uint32_t i = 0u; i < HIGH_PAGE_PIN_SLOTS; ++i) {
        high_page_pins[i].page = 0u;
        high_page_pins[i].references = 0u;
        high_page_pins[i].used = 0u;
        high_page_pins[i].pending_free = 0u;
    }
    for (uint32_t i = 0u; i < HIGH_BITMAP_WORDS; ++i) {
        high_available[i] = 0u;
        high_allocated[i] = 0u;
    }
    total_pages = free_pages = high_free_pages = high_scan_hint = 0u;
    high_available_bytes = 0u;
}

int memory_init(uint32_t mi){
    spinlock_init(&memory_lock);
    memory_reset_unavailable();

    /*
     * Validate the complete Multiboot2 tag stream before consuming any
     * memory-map or module records. A malformed boot handoff must leave every
     * allocator page reserved rather than exposing partially parsed RAM.
     */
    if(multiboot2_info_validate((const void *)(uintptr_t)mi)!=0){
        debug_write("LIONOS:MB2-INFO-INVALID-MEMORY-UNAVAILABLE\\n");
        return -1;
    }

    uint32_t ts = mb2_read_u32((const uint8_t *)(uintptr_t)mi);
    mb2_info_iterator_t iterator;
    mb2_tag_view_t tag;
    if(multiboot2_info_iterator_init((const void *)(uintptr_t)mi,&iterator)!=0)
        return -1;

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
            uint32_t entry_size = mb2_read_u32(tag.payload);
            for (uint32_t offset = 0u; offset < entries_bytes;
                 offset += entry_size) {
                const uint8_t *entry = entries + offset;
                uint64_t region_start = mb2_read_u64(entry);
                uint64_t region_length = mb2_read_u64(entry + 8u);
                uint32_t region_type = mb2_read_u32(entry + 16u);
                if (region_length > UINT64_MAX - region_start)
                    continue;
                uint64_t region_end = region_start + region_length;
                if (pass == 0u) {
                    if (region_type != MULTIBOOT2_MEMORY_AVAILABLE)
                        continue;
                    mark_low_available(region_start, region_end);
                    high_mark_available(region_start, region_end);
                } else if (region_type != MULTIBOOT2_MEMORY_AVAILABLE) {
                    reserve_low_range(region_start, region_end);
                    high_reserve_range(region_start, region_end);
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
        memory_reset_unavailable();
        debug_write("LIONOS:MB2-INFO-ITERATION-FAILED-MEMORY-UNAVAILABLE\\n");
        return -1;
    }

    reserve_low_range(0,RESERVED_PAGES*PAGE_SIZE);
    reserve_low_range((uint32_t)(uintptr_t)&_kernel_start,(uint32_t)(uintptr_t)&_kernel_end);
    reserve_low_range(mi,(uint64_t)mi+ts);
    high_reserve_range(mi,(uint64_t)mi+ts);

    /* Reserve every module only after all available regions were discovered. */
    if(multiboot2_info_iterator_init((const void *)(uintptr_t)mi,&iterator)!=0){
        memory_reset_unavailable();
        debug_write("LIONOS:MB2-MODULE-ITERATION-FAILED\\n");
        return -1;
    }
    while((next=multiboot2_info_iterator_next(&iterator,&tag))>0){
        if(tag.type==MULTIBOOT2_TAG_MODULE){
            uint32_t module_start = mb2_read_u32(tag.payload);
            uint32_t module_end = mb2_read_u32(tag.payload + 4u);
            reserve_low_range(module_start,module_end);
            high_reserve_range(module_start,module_end);
        }
    }
    if(next<0){
        memory_reset_unavailable();
        debug_write("LIONOS:MB2-MODULE-ITERATION-FAILED\\n");
        return -1;
    }

    /*
     * A structurally valid map can still describe no allocatable RAM (for
     * example, an empty map or ranges fully covered by reservations). Do not
     * report a usable memory manager when both allocator windows are empty.
     */
    if (free_pages == 0u && high_free_pages == 0u) {
        memory_reset_unavailable();
        debug_write("LIONOS:NO-USABLE-PHYSICAL-MEMORY\\n");
        return -1;
    }
    return 0;
}

static int high_find_contiguous(uint32_t count, uint32_t *first_page)
{
    if (!count || !first_page || count > HIGH_MEMORY_PAGES)
        return 0;

    /*
     * Prefer the current scan hint, then perform a complete linear pass.
     * The second pass is intentionally allowed to cross the hint boundary:
     * physical contiguity must never be broken by allocator search state.
     */
    for (uint32_t pass = 0u; pass < 2u; ++pass) {
        uint32_t begin = pass == 0u ? high_scan_hint : 0u;
        uint32_t run = 0u;
        uint32_t run_start = 0u;

        for (uint32_t page = begin; page < HIGH_MEMORY_PAGES; ++page) {
            if (high_bit_get(high_available, page) &&
                !high_bit_get(high_allocated, page)) {
                if (run == 0u)
                    run_start = page;
                ++run;
                if (run == count) {
                    *first_page = run_start;
                    return 1;
                }
            } else {
                run = 0u;
            }
        }
    }
    return 0;
}

int page_alloc_high_contiguous(uint32_t count, uint64_t *physical_address)
{
    uint32_t first_page;
    uint32_t irq;

    if (!physical_address || !count)
        return -1;

    irq = spinlock_irqsave_acquire(&memory_lock);
    if (count > high_free_pages ||
        !high_find_contiguous(count, &first_page)) {
        spinlock_irqrestore_release(&memory_lock, irq);
        return -1;
    }

    for (uint32_t i = 0u; i < count; ++i)
        high_bit_set(high_allocated, first_page + i);

    high_free_pages -= count;
    high_scan_hint = first_page + count;
    if (high_scan_hint >= HIGH_MEMORY_PAGES)
        high_scan_hint = 0u;

    *physical_address = HIGH_MEMORY_BASE +
                        (uint64_t)first_page * PAGE_SIZE;
    spinlock_irqrestore_release(&memory_lock, irq);
    return 0;
}

int page_alloc_high(uint64_t *physical_address)
{
    return page_alloc_high_contiguous(1u, physical_address);
}

void page_free_high_contiguous(uint64_t physical_address, uint32_t count)
{
    uint64_t first_page_64;
    uint32_t first_page;
    uint32_t irq;

    if (!count || (physical_address & (PAGE_SIZE - 1u)) ||
        physical_address < HIGH_MEMORY_BASE ||
        physical_address >= HIGH_MEMORY_LIMIT)
        return;

    first_page_64 = (physical_address - HIGH_MEMORY_BASE) / PAGE_SIZE;
    if (first_page_64 >= HIGH_MEMORY_PAGES ||
        (uint64_t)count > (uint64_t)HIGH_MEMORY_PAGES - first_page_64)
        return;

    first_page = (uint32_t)first_page_64;
    irq = spinlock_irqsave_acquire(&memory_lock);
    for (uint32_t i = 0u; i < count; ++i) {
        uint32_t page = first_page + i;
        if (!high_bit_get(high_allocated, page))
            continue;

        int pin_slot = high_pin_find(page);
        if (pin_slot >= 0) {
            high_page_pins[(uint32_t)pin_slot].pending_free = 1u;
            continue;
        }

        high_bit_clear(high_allocated, page);
        if (high_bit_get(high_available, page)) {
            ++high_free_pages;
            if (page < high_scan_hint)
                high_scan_hint = page;
        }
    }
    spinlock_irqrestore_release(&memory_lock, irq);
}

void page_free_high(uint64_t physical_address)
{
    page_free_high_contiguous(physical_address, 1u);
}

int memory_high_page_pin(uint64_t physical_address)
{
    uint32_t page;
    uint32_t irq;
    int free_slot = -1;

    if ((physical_address & (PAGE_SIZE - 1u)) != 0u ||
        physical_address < HIGH_MEMORY_BASE ||
        physical_address >= HIGH_MEMORY_LIMIT)
        return -1;

    page = (uint32_t)((physical_address - HIGH_MEMORY_BASE) / PAGE_SIZE);
    irq = spinlock_irqsave_acquire(&memory_lock);
    if (!high_bit_get(high_available, page) ||
        !high_bit_get(high_allocated, page)) {
        spinlock_irqrestore_release(&memory_lock, irq);
        return -1;
    }

    for (uint32_t i = 0u; i < HIGH_PAGE_PIN_SLOTS; ++i) {
        if (high_page_pins[i].used) {
            if (high_page_pins[i].page == page) {
                /* Once release is requested, no new mapping may prolong life. */
                if (high_page_pins[i].pending_free ||
                    high_page_pins[i].references == UINT16_MAX) {
                    spinlock_irqrestore_release(&memory_lock, irq);
                    return -1;
                }
                ++high_page_pins[i].references;
                spinlock_irqrestore_release(&memory_lock, irq);
                return 0;
            }
        } else if (free_slot < 0) {
            free_slot = (int)i;
        }
    }

    if (free_slot < 0) {
        spinlock_irqrestore_release(&memory_lock, irq);
        return -1;
    }

    high_page_pins[(uint32_t)free_slot].page = page;
    high_page_pins[(uint32_t)free_slot].references = 1u;
    high_page_pins[(uint32_t)free_slot].used = 1u;
    high_page_pins[(uint32_t)free_slot].pending_free = 0u;
    spinlock_irqrestore_release(&memory_lock, irq);
    return 0;
}

int memory_high_page_unpin(uint64_t physical_address)
{
    uint32_t page;
    uint32_t irq;
    int pin_slot;

    if ((physical_address & (PAGE_SIZE - 1u)) != 0u ||
        physical_address < HIGH_MEMORY_BASE ||
        physical_address >= HIGH_MEMORY_LIMIT)
        return -1;

    page = (uint32_t)((physical_address - HIGH_MEMORY_BASE) / PAGE_SIZE);
    irq = spinlock_irqsave_acquire(&memory_lock);
    pin_slot = high_pin_find(page);
    if (pin_slot < 0) {
        spinlock_irqrestore_release(&memory_lock, irq);
        return -1;
    }

    struct high_page_pin_record *record =
        &high_page_pins[(uint32_t)pin_slot];
    if (record->references > 1u) {
        --record->references;
        spinlock_irqrestore_release(&memory_lock, irq);
        return 0;
    }

    if (record->pending_free) {
        high_bit_clear(high_allocated, page);
        if (high_bit_get(high_available, page)) {
            ++high_free_pages;
            if (page < high_scan_hint)
                high_scan_hint = page;
        }
    }

    record->page = 0u;
    record->references = 0u;
    record->used = 0u;
    record->pending_free = 0u;
    spinlock_irqrestore_release(&memory_lock, irq);
    return 0;
}

void memory_reserve_physical_range(uint64_t start,uint64_t end){
    if(end<=start)return;
    uint32_t irq=spinlock_irqsave_acquire(&memory_lock);
    high_reserve_range(start,end);
    reserve_low_range(start, end);
    spinlock_irqrestore_release(&memory_lock,irq);
}

uint32_t memory_high_free_pages(void){
    uint32_t irq=spinlock_irqsave_acquire(&memory_lock);
    uint32_t count=high_free_pages;
    spinlock_irqrestore_release(&memory_lock,irq);
    return count;
}

int memory_high_page_is_allocated(uint64_t physical_address)
{
    uint32_t page;
    uint32_t irq;
    int allocated;

    if ((physical_address & (PAGE_SIZE - 1u)) != 0u ||
        physical_address < HIGH_MEMORY_BASE ||
        physical_address >= HIGH_MEMORY_LIMIT)
        return -1;

    page = (uint32_t)((physical_address - HIGH_MEMORY_BASE) / PAGE_SIZE);
    irq = spinlock_irqsave_acquire(&memory_lock);
    allocated = high_bit_get(high_available, page) &&
                high_bit_get(high_allocated, page);
    spinlock_irqrestore_release(&memory_lock, irq);
    return allocated ? 1 : 0;
}
void*page_alloc_contiguous(uint32_t count){if(!count||count>MAX_PAGES-RESERVED_PAGES)return 0;uint32_t irq=spinlock_irqsave_acquire(&memory_lock);uint32_t run=0,start=RESERVED_PAGES;for(uint32_t p=RESERVED_PAGES;p<MAX_PAGES;++p){if(is_free(p)){if(!run)start=p;if(++run==count){for(uint32_t i=0;i<count;++i){mark_used(start+i);page_refs[start+i]=1u;}free_pages-=count;spinlock_irqrestore_release(&memory_lock,irq);return(void*)(uintptr_t)(start*PAGE_SIZE);}}else run=0;}spinlock_irqrestore_release(&memory_lock,irq);return 0;}
void*page_alloc(void){return page_alloc_contiguous(1);}
int page_alloc_physical(uint64_t *physical_address)
{
    void *low_page;
    uint64_t high_page;

    if (!physical_address)
        return -1;

    /*
     * Prefer the directly identity-mapped low window for existing kernel
     * callers. Fall back to the high allocator only when low memory is
     * exhausted; publish the output only after one allocator succeeds.
     */
    low_page = page_alloc();
    if (low_page != 0) {
        *physical_address = (uint64_t)(uintptr_t)low_page;
        return 0;
    }

    if (page_alloc_high(&high_page) != 0)
        return -1;

    *physical_address = high_page;
    return 0;
}

int page_alloc_physical_contiguous(uint32_t count,
                                   uint64_t *physical_address)
{
    void *low_run;
    uint64_t high_run;

    if (!physical_address || count == 0u)
        return -1;

    /*
     * Keep legacy callers on the identity-mapped low window whenever a
     * complete contiguous run is available. The high allocator is a
     * transactional fallback and publishes no partial result on failure.
     */
    low_run = page_alloc_contiguous(count);
    if (low_run != 0) {
        *physical_address = (uint64_t)(uintptr_t)low_run;
        return 0;
    }

    if (page_alloc_high_contiguous(count, &high_run) != 0)
        return -1;

    *physical_address = high_run;
    return 0;
}

void page_free_physical(uint64_t physical_address)
{
    const uint64_t low_memory_limit = (uint64_t)MAX_PAGES * PAGE_SIZE;

    if ((physical_address & (PAGE_SIZE - 1u)) != 0u)
        return;

    if (physical_address < low_memory_limit) {
        /* The first low pages are reserved and are never allocator-owned. */
        if (physical_address < (uint64_t)RESERVED_PAGES * PAGE_SIZE)
            return;
        page_free((void *)(uintptr_t)physical_address);
        return;
    }

    if (physical_address < HIGH_MEMORY_BASE ||
        physical_address >= HIGH_MEMORY_LIMIT)
        return;

    page_free_high(physical_address);
}

void page_free_physical_contiguous(uint64_t physical_address,
                                   uint32_t count)
{
    const uint64_t low_memory_limit = (uint64_t)MAX_PAGES * PAGE_SIZE;
    const uint64_t low_reserved_limit = (uint64_t)RESERVED_PAGES * PAGE_SIZE;
    uint64_t remaining;
    uint64_t low_count;

    if (count == 0u || (physical_address & (PAGE_SIZE - 1u)) != 0u ||
        physical_address < low_reserved_limit ||
        physical_address >= HIGH_MEMORY_LIMIT ||
        (uint64_t)count > (HIGH_MEMORY_LIMIT - physical_address) / PAGE_SIZE)
        return;

    remaining = count;
    if (physical_address < low_memory_limit) {
        low_count = (low_memory_limit - physical_address) / PAGE_SIZE;
        if (low_count > remaining)
            low_count = remaining;

        for (uint64_t page = 0u; page < low_count; ++page) {
            uint64_t address = physical_address + page * PAGE_SIZE;
            page_free((void *)(uintptr_t)address);
        }

        physical_address += low_count * PAGE_SIZE;
        remaining -= low_count;
    }

    if (remaining != 0u)
        page_free_high_contiguous(physical_address, (uint32_t)remaining);
}
void page_retain(void*ptr){uint32_t a=(uint32_t)(uintptr_t)ptr;if((a&(PAGE_SIZE-1u))!=0)return;uint32_t p=a/PAGE_SIZE;if(p<RESERVED_PAGES||p>=MAX_PAGES)return;uint32_t irq=spinlock_irqsave_acquire(&memory_lock);if(!is_free(p)&&page_refs[p]!=0u&&page_refs[p]<0xFFFFu)++page_refs[p];spinlock_irqrestore_release(&memory_lock,irq);}
uint32_t page_refcount(void*ptr){uint32_t a=(uint32_t)(uintptr_t)ptr;if((a&(PAGE_SIZE-1u))!=0)return 0u;uint32_t p=a/PAGE_SIZE;if(p<RESERVED_PAGES||p>=MAX_PAGES)return 0u;uint32_t irq=spinlock_irqsave_acquire(&memory_lock);uint32_t n=is_free(p)?0u:(uint32_t)page_refs[p];spinlock_irqrestore_release(&memory_lock,irq);return n;}
void page_free(void*ptr){uint32_t a=(uint32_t)(uintptr_t)ptr;if((a&(PAGE_SIZE-1u))!=0)return;uint32_t p=a/PAGE_SIZE;if(p<RESERVED_PAGES||p>=MAX_PAGES)return;uint32_t irq=spinlock_irqsave_acquire(&memory_lock);if(!is_free(p)&&page_refs[p]){if(--page_refs[p]==0u){mark_free(p);++free_pages;}}spinlock_irqrestore_release(&memory_lock,irq);}
uint32_t memory_total_pages(void){return total_pages;}uint32_t memory_free_pages(void){uint32_t irq=spinlock_irqsave_acquire(&memory_lock);uint32_t n=free_pages;spinlock_irqrestore_release(&memory_lock,irq);return n;}uint64_t memory_managed_limit_bytes(void){return HIGH_MEMORY_LIMIT;}

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
