#include <stdint.h>
#include "memory.h"
#include "paging.h"
#include "cpu.h"
#include "lapic.h"
#include "smp.h"
#include "spinlock.h"
#include "debug.h"

#define PAGE_SIZE 4096u
#define PAE_ROOT_ENTRIES 4u
#define PAE_PD_ENTRIES 512u
#define PAE_PT_ENTRIES 512u
#define IDENTITY_PT_COUNT 128u
#define KERNEL_MMIO_BASE 0xF0000000u
#define KERNEL_MMIO_PD_INDEX ((KERNEL_MMIO_BASE >> 21) & 0x1FFu)
/*
 * 0xF0000000..0xFFFFFFFF is 256 MiB. Each page-directory entry covers
 * 2 MiB, so 128 page tables cover this reserved virtual window.
 */
#define KERNEL_MMIO_PT_COUNT 128u
#define USER_LIMIT 0xC0000000u
#define PROCESS_PAGING_PAGES 5u /* PDPT + four page-directory pages */

#define PTE_PRESENT 0x1ULL
#define PTE_WRITABLE 0x2ULL
#define PTE_USER 0x4ULL
#define PTE_COW 0x200ULL
#define PTE_FLAGS_MASK 0x21FULL
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/*
 * LionOS is still a 32-bit protected-mode kernel, but PAE lets the kernel
 * address physical memory above 4 GiB. This matters for modern UEFI/GOP
 * framebuffers, which are commonly placed above the 32-bit physical range.
 *
 * The normal kernel identity map remains 0..256 MiB so existing 32-bit
 * physical allocations stay unchanged. The F0000000..FFFFFFFF virtual range
 * is reserved for MMIO/framebuffer mappings and can point anywhere in the
 * 64-bit physical address space supported by PAE.
 */
static uint64_t page_root[PAE_ROOT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t kernel_pd[PAE_ROOT_ENTRIES][PAE_PD_ENTRIES] __attribute__((aligned(4096)));
static uint64_t identity_pt[IDENTITY_PT_COUNT][PAE_PT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t kernel_mmio_pt[KERNEL_MMIO_PT_COUNT][PAE_PT_ENTRIES] __attribute__((aligned(4096)));
static uint32_t current_directory[LIONOS_MAX_CPUS];
#define LIONOS_TLB_IPI_VECTOR 49u
static struct spinlock kernel_map_lock;
static volatile uint32_t shootdown_address;
static volatile uint32_t shootdown_acks;
/*
 * Keep the temporary high-memory window separate from device mappings:
 * xHCI MMIO: 0xF1000000..0xF10FFFFF
 * high-memory window: 0xF2000000..0xF3FFFFFF
 * framebuffer: starts at 0xF4000000
 * Intel GPU MMIO: starts at 0xF8000000
 */
#define HIGH_MAP_BASE 0xF2000000u
#define HIGH_MAP_PAGES 0x2000u /* 32 MiB; ends immediately before framebuffer space. */
static uint32_t high_map_bitmap[HIGH_MAP_PAGES/32u];
static struct spinlock high_map_lock;
static uint32_t *current_directory_slot(void){uint32_t cpu=cpu_current_index();return &current_directory[cpu<LIONOS_MAX_CPUS?cpu:0u];}

static uint64_t *pdpt_ptr(uint32_t physical) {
    return (uint64_t *)(uintptr_t)physical;
}

static uint64_t *pd_ptr(uint32_t physical, uint32_t pdpt_index) {
    uint64_t *root = pdpt_ptr(physical);
    return (uint64_t *)(uintptr_t)(root[pdpt_index] & PTE_ADDR_MASK);
}

static uint64_t *pt_ptr(uint64_t entry) {
    return (uint64_t *)(uintptr_t)(entry & PTE_ADDR_MASK);
}

static void pte_store_atomic(volatile uint64_t *entry,uint64_t value){
    uint32_t low=(uint32_t)value;
    uint32_t high=(uint32_t)(value>>32);
    __asm__ volatile(
        "1: movl (%1), %%eax\n"
        "movl 4(%1), %%edx\n"
        "lock; cmpxchg8b (%1)\n"
        "jnz 1b\n"
        : "+m"(*entry)
        : "r"(entry), "b"(low), "c"(high)
        : "eax","edx","memory","cc");
}

static void zero_words(uint64_t *buffer, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) buffer[i] = 0;
}

static void enable_pae_paging(void) {
    uint32_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 0x20u; /* CR4.PAE */
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");

    __asm__ volatile ("mov %0, %%cr3" : : "r"(*current_directory_slot()) : "memory");

    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80010000u; /* CR0.PG | CR0.WP: kernel cannot bypass user read-only/COW pages. */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

void paging_init(void) {
    spinlock_init(&kernel_map_lock);
    shootdown_address=0u;
    shootdown_acks=0u;
    for(uint32_t i=0u;i<HIGH_MAP_PAGES/32u;++i)high_map_bitmap[i]=0u;
    spinlock_init(&high_map_lock);
    zero_words(page_root, PAE_ROOT_ENTRIES);
    for (uint32_t i = 0; i < PAE_ROOT_ENTRIES; ++i)
        zero_words(kernel_pd[i], PAE_PD_ENTRIES);
    for (uint32_t i = 0; i < IDENTITY_PT_COUNT; ++i)
        zero_words(identity_pt[i], PAE_PT_ENTRIES);
    for (uint32_t i = 0; i < KERNEL_MMIO_PT_COUNT; ++i)
        zero_words(kernel_mmio_pt[i], PAE_PT_ENTRIES);

    /* Identity-map the low 256 MiB with 4 KiB pages. */
    for (uint32_t table = 0; table < IDENTITY_PT_COUNT; ++table) {
        for (uint32_t page = 0; page < PAE_PT_ENTRIES; ++page) {
            uint64_t physical = ((uint64_t)table * PAE_PT_ENTRIES + page) * PAGE_SIZE;
            identity_pt[table][page] = physical | PTE_PRESENT | PTE_WRITABLE;
        }
        kernel_pd[0][table] =
            ((uint64_t)(uintptr_t)identity_pt[table] & PTE_ADDR_MASK) |
            PTE_PRESENT | PTE_WRITABLE;
    }

    /*
     * Map the F0000000..FFFFFFFF virtual range for MMIO/framebuffers.
     * This reserves 128 MiB = 64 page-directory entries for framebuffer and MMIO mappings.
     */
    for (uint32_t table = 0; table < KERNEL_MMIO_PT_COUNT; ++table) {
        kernel_pd[3][KERNEL_MMIO_PD_INDEX + table] =
            ((uint64_t)(uintptr_t)kernel_mmio_pt[table] & PTE_ADDR_MASK) |
            PTE_PRESENT | PTE_WRITABLE;
    }

    page_root[0] = ((uint64_t)(uintptr_t)kernel_pd[0] & PTE_ADDR_MASK) | PTE_PRESENT;
    page_root[1] = ((uint64_t)(uintptr_t)kernel_pd[1] & PTE_ADDR_MASK) | PTE_PRESENT;
    page_root[2] = ((uint64_t)(uintptr_t)kernel_pd[2] & PTE_ADDR_MASK) | PTE_PRESENT;
    page_root[3] = ((uint64_t)(uintptr_t)kernel_pd[3] & PTE_ADDR_MASK) | PTE_PRESENT;

    current_directory[0] = (uint32_t)(uintptr_t)page_root;
    enable_pae_paging();
}

uint32_t paging_kernel_directory(void) {
    return (uint32_t)(uintptr_t)page_root;
}

void paging_tlb_shootdown_interrupt(void) {
    uint32_t address=__atomic_load_n(&shootdown_address,__ATOMIC_ACQUIRE);
    __asm__ volatile ("invlpg (%0)" : : "r"(address) : "memory");
    __atomic_add_fetch(&shootdown_acks,1u,__ATOMIC_RELEASE);
    lapic_eoi();
}

static void paging_kernel_tlb_shootdown(uint32_t address) {
    uint32_t self=cpu_current_index();
    uint32_t targets=0u;
    for(uint32_t i=0u;i<cpu_count_hint()&&i<LIONOS_MAX_CPUS;++i){
        const struct cpu_info *info=cpu_get(i);
        if(i!=self&&info&&info->online&&info->apic_id<=0xFFu)++targets;
    }
    if(!targets)return;
    __atomic_store_n(&shootdown_acks,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&shootdown_address,address,__ATOMIC_RELEASE);
    for(uint32_t i=0u;i<cpu_count_hint()&&i<LIONOS_MAX_CPUS;++i){
        const struct cpu_info *info=cpu_get(i);
        if(i!=self&&info&&info->online&&info->apic_id<=0xFFu)
            lapic_send_ipi(info->apic_id,LIONOS_TLB_IPI_VECTOR);
    }
    /* A stale translation is unsafe to reuse; do not return until every online
       target has acknowledged the invalidation. */
    while(__atomic_load_n(&shootdown_acks,__ATOMIC_ACQUIRE)<targets)
        __asm__ volatile("pause" ::: "memory");
    debug_write("LIONOS:TLB-SHOOTDOWN-ACKED\n");
}

int paging_map_kernel_page(uint32_t virtual_address, uint64_t physical_address, uint32_t flags) {
    /* Reject addresses beyond the PAE PTE address field instead of silently truncating them. */
    if ((virtual_address & (PAGE_SIZE - 1u)) ||
        (physical_address & (PAGE_SIZE - 1u)) ||
        (physical_address & ~PTE_ADDR_MASK)) return -1;
    if (virtual_address < KERNEL_MMIO_BASE) return -1;
    /* Kernel-wide MMIO mappings are serialized by a BSP-owned IPI transaction. */
    if (cpu_current_index() != 0u) return -1;

    uint32_t pdpt_index = virtual_address >> 30;
    uint32_t pd_index = (virtual_address >> 21) & 0x1FFu;
    uint32_t pt_index = (virtual_address >> 12) & 0x1FFu;
    if (pdpt_index != 3u || pd_index < KERNEL_MMIO_PD_INDEX) return -1;

    uint32_t mmio_index = pd_index - KERNEL_MMIO_PD_INDEX;
    if (mmio_index >= KERNEL_MMIO_PT_COUNT) return -1;

    uint32_t irq=spinlock_irqsave_acquire(&kernel_map_lock);
    uint64_t *table = kernel_mmio_pt[mmio_index];
    pte_store_atomic(&table[pt_index],(physical_address & PTE_ADDR_MASK) |
                      ((uint64_t)flags & PTE_FLAGS_MASK) |
                      PTE_PRESENT);
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
    paging_kernel_tlb_shootdown(virtual_address);
    spinlock_irqrestore_release(&kernel_map_lock,irq);
    return 0;
}

int paging_map_high_page(uint64_t physical_address,uint32_t flags,uint32_t *virtual_address){
    if(!virtual_address||(physical_address& (PAGE_SIZE-1u))||
       physical_address<0x10000000ULL||(physical_address&~PTE_ADDR_MASK))return -1;
    if(cpu_current_index()!=0u)return -1;
    uint32_t irq=spinlock_irqsave_acquire(&high_map_lock);
    uint32_t slot=HIGH_MAP_PAGES;
    for(uint32_t i=0u;i<HIGH_MAP_PAGES;++i){
        if(!(high_map_bitmap[i>>5]&(1u<<(i&31u)))){slot=i;break;}
    }
    if(slot==HIGH_MAP_PAGES){spinlock_irqrestore_release(&high_map_lock,irq);return -1;}
    high_map_bitmap[slot>>5]|=1u<<(slot&31u);
    spinlock_irqrestore_release(&high_map_lock,irq);
    uint32_t address=HIGH_MAP_BASE+slot*PAGE_SIZE;
    if(paging_map_kernel_page(address,physical_address,flags)!=0){
        irq=spinlock_irqsave_acquire(&high_map_lock);
        high_map_bitmap[slot>>5]&=~(1u<<(slot&31u));
        spinlock_irqrestore_release(&high_map_lock,irq);
        return -1;
    }
    *virtual_address=address;
    return 0;
}

int paging_unmap_high_page(uint32_t virtual_address){
    if(virtual_address<HIGH_MAP_BASE||
       virtual_address>=HIGH_MAP_BASE+HIGH_MAP_PAGES*PAGE_SIZE||
       (virtual_address&(PAGE_SIZE-1u))||cpu_current_index()!=0u)return -1;
    uint32_t slot=(virtual_address-HIGH_MAP_BASE)/PAGE_SIZE;
    uint32_t irq=spinlock_irqsave_acquire(&high_map_lock);
    if(!(high_map_bitmap[slot>>5]&(1u<<(slot&31u)))){
        spinlock_irqrestore_release(&high_map_lock,irq);
        return -1;
    }
    spinlock_irqrestore_release(&high_map_lock,irq);
    uint32_t map_irq=spinlock_irqsave_acquire(&kernel_map_lock);
    uint32_t pd_index=(virtual_address>>21)&0x1FFu;
    uint32_t pt_index=(virtual_address>>12)&0x1FFu;
    uint32_t mmio_index=pd_index-KERNEL_MMIO_PD_INDEX;
    if(mmio_index>=KERNEL_MMIO_PT_COUNT){
        spinlock_irqrestore_release(&kernel_map_lock,map_irq);
        return -1;
    }
    pte_store_atomic(&kernel_mmio_pt[mmio_index][pt_index],0u);
    __asm__ volatile("invlpg (%0)"::"r"(virtual_address):"memory");
    paging_kernel_tlb_shootdown(virtual_address);
    spinlock_irqrestore_release(&kernel_map_lock,map_irq);
    irq=spinlock_irqsave_acquire(&high_map_lock);
    high_map_bitmap[slot>>5]&=~(1u<<(slot&31u));
    spinlock_irqrestore_release(&high_map_lock,irq);
    return 0;
}

uint32_t paging_create_address_space(void) {
    void *block = page_alloc_contiguous(PROCESS_PAGING_PAGES);
    if (!block) return 0;

    uint32_t base = (uint32_t)(uintptr_t)block;
    uint64_t *root = (uint64_t *)(uintptr_t)base;
    for (uint32_t i = 0; i < PROCESS_PAGING_PAGES * PAGE_SIZE / sizeof(uint64_t); ++i)
        root[i] = 0;

    for (uint32_t i = 0; i < PAE_ROOT_ENTRIES; ++i) {
        uint64_t *pd = (uint64_t *)(uintptr_t)(base + (i + 1u) * PAGE_SIZE);
        for (uint32_t j = 0; j < PAE_PD_ENTRIES; ++j)
            pd[j] = kernel_pd[i][j] & ~PTE_USER;
        root[i] = ((uint64_t)(uintptr_t)pd & PTE_ADDR_MASK) | PTE_PRESENT;
    }

    return base;
}

int paging_map_user_page_in(uint32_t pd_physical, uint32_t virtual_address,
                            uint32_t physical_address, uint32_t flags) {
    if (!pd_physical || virtual_address >= USER_LIMIT ||
        (virtual_address & (PAGE_SIZE - 1u)) || (physical_address & (PAGE_SIZE - 1u))) return -1;

    uint32_t pdpt_index = virtual_address >> 30;
    uint32_t pd_index = (virtual_address >> 21) & 0x1FFu;
    uint32_t pt_index = (virtual_address >> 12) & 0x1FFu;

    uint64_t *directory = pd_ptr(pd_physical, pdpt_index);
    if (!directory) return -1;

    uint64_t pde = directory[pd_index];
    uint64_t *table;
    if ((pde & PTE_PRESENT) && (pde & PTE_USER)) {
        table = pt_ptr(pde);
    } else {
        table = (uint64_t *)page_alloc();
        if (!table) return -1;
        zero_words(table, PAE_PT_ENTRIES);
        directory[pd_index] =
            ((uint64_t)(uintptr_t)table & PTE_ADDR_MASK) |
            PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    table[pt_index] = ((uint64_t)physical_address & PTE_ADDR_MASK) |
                      ((uint64_t)flags & PTE_FLAGS_MASK) |
                      PTE_PRESENT | PTE_USER;

    if (*current_directory_slot() == pd_physical)
        __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
    return 0;
}

int paging_set_user_page_flags(uint32_t pd_physical,uint32_t virtual_address,uint32_t flags){
    if(!pd_physical||virtual_address>=USER_LIMIT||(virtual_address&(PAGE_SIZE-1u)))return-1;
    uint32_t pdpt_index=virtual_address>>30,pd_index=(virtual_address>>21)&0x1FFu,pt_index=(virtual_address>>12)&0x1FFu;
    uint64_t*directory=pd_ptr(pd_physical,pdpt_index);if(!directory)return-1;uint64_t pde=directory[pd_index];
    if(!(pde&PTE_PRESENT)||!(pde&PTE_USER))return-1;
    uint64_t*table=pt_ptr(pde);uint64_t pte=table[pt_index];
    if(!(pte&PTE_PRESENT)||!(pte&PTE_USER))return-1;
    table[pt_index]=(pte&PTE_ADDR_MASK)|((uint64_t)flags&PTE_FLAGS_MASK)|PTE_PRESENT|PTE_USER;
    if(pd_physical==paging_current_address_space())__asm__ volatile("invlpg (%0)"::"r"(virtual_address):"memory");
    return 0;
}
int paging_mark_cow(uint32_t pd_physical,uint32_t virtual_address){
    uint32_t phys=0,flags=0;
    if(paging_get_user_page(pd_physical,virtual_address,&phys,&flags)!=0)return-1;
    if(!(flags&0x2u))return-1;
    flags=(flags&~0x2u)|PTE_COW;
    return paging_set_user_page_flags(pd_physical,virtual_address,flags);
}
int paging_resolve_cow(uint32_t pd_physical,uint32_t virtual_address){
    if(!pd_physical||virtual_address>=USER_LIMIT||(virtual_address&(PAGE_SIZE-1u)))return-1;
    uint32_t pdpt_index=virtual_address>>30,pd_index=(virtual_address>>21)&0x1FFu,pt_index=(virtual_address>>12)&0x1FFu;
    uint64_t*directory=pd_ptr(pd_physical,pdpt_index);if(!directory)return-1;
    uint64_t pde=directory[pd_index];if(!(pde&PTE_PRESENT)||!(pde&PTE_USER))return-1;
    uint64_t*table=pt_ptr(pde);uint64_t pte=table[pt_index];
    if(!(pte&PTE_PRESENT)||!(pte&PTE_USER)||!(pte&PTE_COW))return-1;
    uint32_t old_phys=(uint32_t)(pte&PTE_ADDR_MASK),old_flags=(uint32_t)(pte&PTE_FLAGS_MASK);
    uint32_t refs=page_refcount((void*)(uintptr_t)old_phys);if(!refs)return-1;
    if(refs==1u){
        table[pt_index]=(pte&PTE_ADDR_MASK)|((uint64_t)((old_flags|0x2u)&~(uint32_t)PTE_COW))|PTE_PRESENT|PTE_USER;
    }else{
        void*new_page=page_alloc();if(!new_page)return-1;
        uint8_t*dst=(uint8_t*)new_page;const uint8_t*src=(const uint8_t*)(uintptr_t)old_phys;
        for(uint32_t i=0u;i<PAGE_SIZE;++i)dst[i]=src[i];
        table[pt_index]=((uint64_t)(uintptr_t)new_page&PTE_ADDR_MASK)|((uint64_t)((old_flags|0x2u)&~(uint32_t)PTE_COW))|PTE_PRESENT|PTE_USER;
        page_free((void*)(uintptr_t)old_phys);
    }
    if(pd_physical==paging_current_address_space())__asm__ volatile("invlpg (%0)"::"r"(virtual_address):"memory");
    return 0;
}

int paging_get_user_page(uint32_t pd_physical, uint32_t virtual_address,
                         uint32_t *physical_address, uint32_t *flags) {
    if (!pd_physical || virtual_address >= USER_LIMIT) return -1;

    uint32_t pdpt_index = virtual_address >> 30;
    uint32_t pd_index = (virtual_address >> 21) & 0x1FFu;
    uint32_t pt_index = (virtual_address >> 12) & 0x1FFu;

    uint64_t *directory = pd_ptr(pd_physical, pdpt_index);
    if (!directory) return -1;
    uint64_t pde = directory[pd_index];
    if (!(pde & PTE_PRESENT) || !(pde & PTE_USER)) return -1;

    uint64_t *table = pt_ptr(pde);
    uint64_t pte = table[pt_index];
    if (!(pte & PTE_PRESENT) || !(pte & PTE_USER)) return -1;

    if (physical_address) *physical_address = (uint32_t)(pte & PTE_ADDR_MASK);
    if (flags) *flags = (uint32_t)(pte & PTE_FLAGS_MASK);
    return 0;
}

void paging_destroy_address_space(uint32_t pd_physical) {
    if (!pd_physical || pd_physical == paging_kernel_directory()) return;

    uint32_t base = pd_physical;
    uint64_t *root = (uint64_t *)(uintptr_t)base;
    for (uint32_t i = 0; i < PAE_ROOT_ENTRIES; ++i) {
        uint64_t *directory = (uint64_t *)(uintptr_t)(base + (i + 1u) * PAGE_SIZE);
        for (uint32_t j = 0; j < PAE_PD_ENTRIES; ++j) {
            uint64_t pde = directory[j];
            /*
             * Kernel-shared mappings are supervisor-only and therefore have
             * PTE_USER clear. Only private userspace page tables are freed.
             */
            if ((pde & (PTE_PRESENT | PTE_USER)) == (PTE_PRESENT | PTE_USER))
                page_free((void *)(uintptr_t)(pde & PTE_ADDR_MASK));
        }
        root[i] = 0;
    }

    for (uint32_t i = 0; i < PROCESS_PAGING_PAGES; ++i)
        page_free((void *)(uintptr_t)(base + i * PAGE_SIZE));
}

void paging_switch_address_space(uint32_t pd_physical) {
    if (!pd_physical || pd_physical == *current_directory_slot()) return;
    *current_directory_slot() = pd_physical;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pd_physical) : "memory");
}

uint32_t paging_current_address_space(void) {
    return *current_directory_slot();
}
