#include <stdint.h>
#include "cpu.h"
#include "lapic.h"
#include "paging.h"
#include "debug.h"

#define MB2_TAG_END 0u
#define MB2_TAG_ACPI_OLD 14u
#define MB2_TAG_ACPI_NEW 15u
#define ACPI_MAX_CPUS LIONOS_MAX_CPUS
#define ACPI_ROOT_WINDOW 0xF0000000u
#define ACPI_CHILD_WINDOW 0xF0800000u
#define ACPI_WINDOW_SIZE 0x00800000u
#define ACPI_PAGE_SIZE 4096u
#define ACPI_PTE_FLAGS 0x3u

struct mb2_tag { uint32_t type,size; };
struct acpi_rsdp20 { char sig[8]; uint8_t checksum; char oem[6]; uint8_t revision; uint32_t rsdt; uint32_t length; uint64_t xsdt; uint8_t ext_checksum; uint8_t reserved[3]; } __attribute__((packed));
struct acpi_sdt { char sig[4]; uint32_t length; uint8_t revision,checksum; char oem_id[6]; char oem_table[8]; uint32_t oem_revision,creator_id,creator_revision; } __attribute__((packed));
struct acpi_madt { struct acpi_sdt h; uint32_t lapic_address,flags; } __attribute__((packed));

static struct cpu_info cpus[LIONOS_MAX_CPUS];
static uint32_t cpu_hint=1u;
static uint32_t cpu_features;
static cpu_identification_t cpu_identification;

static void cpuid_count(uint32_t leaf,uint32_t subleaf,
                        uint32_t *a,uint32_t *b,uint32_t *c,uint32_t *d){
    __asm__ volatile("cpuid":"=a"(*a),"=b"(*b),"=c"(*c),"=d"(*d)
                     :"a"(leaf),"c"(subleaf));
}
#define CPU_FEAT_SSE   (1u<<0)
#define CPU_FEAT_SSE2  (1u<<1)
#define CPU_FEAT_SSE3  (1u<<2)
#define CPU_FEAT_SSSE3 (1u<<3)
#define CPU_FEAT_SSE41 (1u<<4)
#define CPU_FEAT_SSE42 (1u<<5)
#define CPU_FEAT_AVX   (1u<<6)
#define CPU_FEAT_APIC  (1u<<7)

static uint8_t checksum8(const uint8_t*p,uint32_t n){uint8_t s=0;for(uint32_t i=0;i<n;++i)s=(uint8_t)(s+p[i]);return s;}
static int sig4(const char*p,const char*s){return p[0]==s[0]&&p[1]==s[1]&&p[2]==s[2]&&p[3]==s[3];}
static int text_contains(const char *text,const char *needle){
    if(!text||!needle||!needle[0])return 0;
    for(;*text;++text){
        uint32_t i=0u;
        while(needle[i]&&text[i]==needle[i])++i;
        if(!needle[i])return 1;
    }
    return 0;
}
static void reset_cpus(void){for(uint32_t i=0;i<LIONOS_MAX_CPUS;++i){cpus[i].index=i;cpus[i].apic_id=0xFFFFFFFFu;cpus[i].logical_per_package=1u;cpus[i].online=0u;}}
static int acpi_identity_range_ok(uint32_t address,uint32_t length){
    const uint32_t limit=0x10000000u;
    return address&&address<limit&&length<=limit-address;
}

static int acpi_map_sdt(uint64_t physical,uint32_t window,const char *signature,
                        const struct acpi_sdt **mapped_out){
    if(!physical||physical>0x000FFFFFFFFFFFFFULL)return -1;
    if(physical<0x10000000ULL){
        const struct acpi_sdt *direct=(const struct acpi_sdt *)(uintptr_t)(uint32_t)physical;
        if(!acpi_identity_range_ok((uint32_t)physical,sizeof(struct acpi_sdt))||
           direct->length<sizeof(struct acpi_sdt)||
           !acpi_identity_range_ok((uint32_t)physical,direct->length)||
           !sig4(direct->sig,signature)||
           checksum8((const uint8_t *)direct,direct->length)!=0u)return -1;
        if(mapped_out)*mapped_out=direct;
        return 0;
    }
    uint64_t base=physical&~(uint64_t)(ACPI_PAGE_SIZE-1u);
    uint32_t offset=(uint32_t)(physical&(ACPI_PAGE_SIZE-1u));
    if(paging_map_kernel_page(window,base,ACPI_PTE_FLAGS)!=0)return -1;
    if(offset>ACPI_PAGE_SIZE-sizeof(struct acpi_sdt)){
        if(base>0x000FFFFFFFFFF000ULL-ACPI_PAGE_SIZE||
           paging_map_kernel_page(window+ACPI_PAGE_SIZE,base+ACPI_PAGE_SIZE,ACPI_PTE_FLAGS)!=0)
            return -1;
    }
    const struct acpi_sdt *header=(const struct acpi_sdt *)(uintptr_t)(window+offset);
    if(header->length<sizeof(struct acpi_sdt)||
       header->length>ACPI_WINDOW_SIZE-offset)return -1;
    uint32_t mapped_bytes=(offset+header->length+ACPI_PAGE_SIZE-1u)&~(ACPI_PAGE_SIZE-1u);
    for(uint32_t off=ACPI_PAGE_SIZE;off<mapped_bytes;off+=ACPI_PAGE_SIZE){
        if(base>0x000FFFFFFFFFF000ULL-off||
           paging_map_kernel_page(window+off,base+off,ACPI_PTE_FLAGS)!=0)return -1;
    }
    if(!sig4(header->sig,signature)||
       checksum8((const uint8_t *)header,header->length)!=0u)return -1;
    if(mapped_out)*mapped_out=header;
    return 0;
}

static uint32_t acpi_find_madt(uint32_t rsdt,uint64_t xsdt,uint8_t revision){
    const struct acpi_sdt *root=0;
    if(revision>=2u&&xsdt&&
       acpi_map_sdt(xsdt,ACPI_ROOT_WINDOW,"XSDT",&root)==0){
        uint32_t count=(root->length-(uint32_t)sizeof(struct acpi_sdt))/8u;
        const uint8_t *entries=(const uint8_t *)root+sizeof(struct acpi_sdt);
        for(uint32_t i=0u;i<count;++i){
            uint64_t address=*(const uint64_t *)(const void *)(entries+i*8u);
            const struct acpi_sdt *table=0;
            if(acpi_map_sdt(address,ACPI_CHILD_WINDOW,"APIC",&table)==0){
                debug_write("LIONOS:ACPI-MADT-MAPPED\n");
                return (uint32_t)(uintptr_t)table;
            }
        }
    }
    if(rsdt&&acpi_map_sdt((uint64_t)rsdt,ACPI_ROOT_WINDOW,"RSDT",&root)==0){
        uint32_t count=(root->length-(uint32_t)sizeof(struct acpi_sdt))/4u;
        const uint8_t *entries=(const uint8_t *)root+sizeof(struct acpi_sdt);
        for(uint32_t i=0u;i<count;++i){
            uint32_t address=*(const uint32_t *)(const void *)(entries+i*4u);
            const struct acpi_sdt *table=0;
            if(acpi_map_sdt((uint64_t)address,ACPI_CHILD_WINDOW,"APIC",&table)==0){
                debug_write("LIONOS:ACPI-MADT-MAPPED\n");
                return (uint32_t)(uintptr_t)table;
            }
        }
    }
    debug_write("LIONOS:ACPI-MADT-NOT-FOUND\n");
    return 0u;
}
static uint32_t acpi_enumerate(uint32_t mb2,uint32_t bsp_apic){
    if(!mb2)return 0u;
    uint32_t total=*(uint32_t*)(uintptr_t)mb2;
    uint8_t*tag=(uint8_t*)(uintptr_t)mb2+8u;
    uint8_t*end=(uint8_t*)(uintptr_t)mb2+total;
    uint32_t madt=0u;
    while(tag+8u<=end){
        struct mb2_tag*t=(struct mb2_tag*)tag;
        if(t->type==MB2_TAG_END)break;
        if((t->type==MB2_TAG_ACPI_OLD||t->type==MB2_TAG_ACPI_NEW)&&t->size>=16u){
            const struct acpi_rsdp20*r=(const struct acpi_rsdp20*)(tag+8u);
            if(r->revision>=2u&&t->size>=8u+36u&&checksum8((const uint8_t*)r,36u)==0u)
                madt=acpi_find_madt(r->rsdt,r->xsdt,r->revision);
            else if(t->size>=8u+20u&&checksum8((const uint8_t*)r,20u)==0u)
                madt=acpi_find_madt(r->rsdt,0,1u);
            if(madt)break;
        }
        tag+=(t->size+7u)&~7u;
    }
    if(!madt)return 0u;

    const struct acpi_madt*m=(const struct acpi_madt*)(uintptr_t)madt;
    if(m->h.length<44u)return 0u;

    uint32_t found=0u,bsp_index=0xFFFFFFFFu;
    const uint8_t*p=(const uint8_t*)m+44u;
    const uint8_t*endm=(const uint8_t*)m+m->h.length;
    while(p+2u<=endm){
        uint8_t type=p[0],len=p[1];
        if(len<2u||p+len>endm)break;
        if(type==0u&&len>=8u){
            uint32_t apic=p[3],flags=*(const uint32_t*)(const void*)(p+4u);
            if((flags&1u)&&found<ACPI_MAX_CPUS){cpus[found].apic_id=apic;cpus[found].logical_per_package=1u;if(apic==bsp_apic)bsp_index=found;++found;}
        }else if(type==9u&&len>=16u){
            uint32_t apic=*(const uint32_t*)(const void*)(p+4u),flags=*(const uint32_t*)(const void*)(p+8u);
            if((flags&1u)&&found<ACPI_MAX_CPUS){cpus[found].apic_id=apic;cpus[found].logical_per_package=1u;if(apic==bsp_apic)bsp_index=found;++found;}
        }
        p+=len;
    }
    if(!found||bsp_index==0xFFFFFFFFu){reset_cpus();return 0u;}
    if(bsp_index!=0u){struct cpu_info tmp=cpus[0];cpus[0]=cpus[bsp_index];cpus[bsp_index]=tmp;}
    for(uint32_t i=0;i<found;++i){cpus[i].index=i;cpus[i].online=(i==0u);}
    return found;
}
void cpu_init(uint32_t multiboot_info){
    reset_cpus();
    uint32_t a,b,c,d;
    cpuid_count(0u,0u,&a,&b,&c,&d);
    uint32_t max_leaf=a;
    cpu_identification=(cpu_identification_t){0};
    cpu_identification.max_basic_leaf=max_leaf;
    for(uint32_t i=0u;i<3u;++i){
        uint32_t word=(i==0u)?b:(i==1u)?d:c;
        for(uint32_t j=0u;j<4u;++j)
            cpu_identification.vendor[i*4u+j]=(char)(word>>(j*8u));
    }
    cpu_features=0u;
    cpuid_count(1u,0u,&a,&b,&c,&d);
    uint32_t leaf1_b=b,leaf1_c=c,leaf1_d=d;
    cpu_identification.signature=a;
    cpu_identification.stepping=(uint8_t)(a&0xFu);
    {
        uint32_t base_family=(a>>8)&0xFu;
        uint32_t base_model=(a>>4)&0xFu;
        uint32_t ext_family=(a>>20)&0xFFu;
        uint32_t ext_model=(a>>16)&0xFu;
        cpu_identification.family=(uint16_t)(base_family==0xFu?base_family+ext_family:base_family);
        cpu_identification.model=(uint16_t)(((base_family==0x6u||base_family==0xFu)
            ?(base_model|(ext_model<<4)):base_model));
    }
    cpu_identification.logical_processors=(uint8_t)((leaf1_b>>16)&0xFFu);
    if(!cpu_identification.logical_processors)cpu_identification.logical_processors=1u;
    cpu_identification.apic_id=(uint8_t)(leaf1_b>>24);
    cpu_identification.physical_cores=0u;
    if(max_leaf>=4u){
        for(uint32_t sub=0u;sub<8u;++sub){
            uint32_t ca,cb,cc,cd;
            cpuid_count(4u,sub,&ca,&cb,&cc,&cd);
            uint32_t type=ca&0x1Fu;
            if(!type)break;
            uint32_t level=(ca>>5)&7u;
            uint64_t line=(cb&0xFFFu)+1u;
            uint64_t partitions=((cb>>12)&0x3FFu)+1u;
            uint64_t ways=((cb>>22)&0x3FFu)+1u;
            uint64_t sets=(uint64_t)cc+1u;
            uint32_t kb=(uint32_t)((line*partitions*ways*sets)/1024u);
            if(level==1u){
                if(type==1u||type==3u)cpu_identification.l1_data_kb+=kb;
                if(type==2u||type==3u)cpu_identification.l1_instruction_kb+=kb;
            }else if(level==2u)cpu_identification.l2_kb+=kb;
            else if(level==3u)cpu_identification.l3_kb+=kb;
        }
    }
    /*
     * CPUID topology level shifts describe x2APIC ID layout, not the number
     * of physical cores on a hybrid processor. Use the maximum logical
     * processor count reported by topology enumeration, but do not derive
     * physical cores from the shift-width difference.
     */
    {
        uint32_t package_logical = 0u;
        uint32_t topology_leaf = (max_leaf >= 0x1Fu) ? 0x1Fu : 0x0Bu;
        if (max_leaf >= 0x1Fu || max_leaf >= 0x0Bu) {
            for (uint32_t sub = 0u; sub < 8u; ++sub) {
                uint32_t ta, tb, tc, td;
                cpuid_count(topology_leaf, sub, &ta, &tb, &tc, &td);
                uint32_t level_type = (tc >> 8) & 0xFFu;
                uint32_t level_count = tb & 0xFFFFu;
                if (!level_type || !level_count)
                    break;
                if (level_count > package_logical)
                    package_logical = level_count;
            }
        }
        if (!package_logical && topology_leaf == 0x1Fu && max_leaf >= 0x0Bu) {
            for (uint32_t sub = 0u; sub < 8u; ++sub) {
                uint32_t ta, tb, tc, td;
                cpuid_count(0x0Bu, sub, &ta, &tb, &tc, &td);
                uint32_t level_type = (tc >> 8) & 0xFFu;
                uint32_t level_count = tb & 0xFFFFu;
                if (!level_type || !level_count)
                    break;
                if (level_count > package_logical)
                    package_logical = level_count;
            }
        }
        if (package_logical) {
            cpu_identification.logical_processors =
                (uint8_t)(package_logical > 255u ? 255u : package_logical);
        }
    }
    if(max_leaf>=0x16u){
        uint32_t fa,fb,fc,fd;
        cpuid_count(0x16u,0u,&fa,&fb,&fc,&fd);
        cpu_identification.base_mhz=(uint16_t)fa;
        cpu_identification.max_mhz=(uint16_t)fb;
        cpu_identification.bus_mhz=(uint16_t)fc;
    }
    if (leaf1_d&(1u<<9)) cpu_features|=CPU_FEAT_APIC;
    if (leaf1_d&(1u<<25)) cpu_features|=CPU_FEAT_SSE;
    if (leaf1_d&(1u<<26)) cpu_features|=CPU_FEAT_SSE2;
    if (leaf1_c&(1u<<0)) cpu_features|=CPU_FEAT_SSE3;
    if (leaf1_c&(1u<<9)) cpu_features|=CPU_FEAT_SSSE3;
    if (leaf1_c&(1u<<19)) cpu_features|=CPU_FEAT_SSE41;
    if (leaf1_c&(1u<<20)) cpu_features|=CPU_FEAT_SSE42;
    if ((leaf1_c&(1u<<27)) && (leaf1_c&(1u<<28))) cpu_features|=CPU_FEAT_AVX;
    uint32_t logical=cpu_identification.logical_processors;
    if(!logical)logical=1u;
    uint32_t bsp=(leaf1_b>>24)&0xFFu;
    cpu_hint=logical;
    if(cpu_hint>LIONOS_MAX_CPUS)cpu_hint=LIONOS_MAX_CPUS;
    cpus[0].apic_id=bsp;cpus[0].logical_per_package=cpu_hint;cpus[0].online=1u;
    cpu_identification.feature_flags=cpu_features;
    cpuid_count(0x80000000u,0u,&a,&b,&c,&d);
    cpu_identification.max_extended_leaf=a;
    if(a>=0x80000004u){
        uint32_t words[12],wa,wb,wc,wd;
        cpuid_count(0x80000002u,0u,&wa,&wb,&wc,&wd);
        words[0]=wa;words[1]=wb;words[2]=wc;words[3]=wd;
        cpuid_count(0x80000003u,0u,&wa,&wb,&wc,&wd);
        words[4]=wa;words[5]=wb;words[6]=wc;words[7]=wd;
        cpuid_count(0x80000004u,0u,&wa,&wb,&wc,&wd);
        words[8]=wa;words[9]=wb;words[10]=wc;words[11]=wd;
        for(uint32_t i=0u;i<12u;++i)
            for(uint32_t j=0u;j<4u;++j)
                cpu_identification.brand[i*4u+j]=(char)(words[i]>>(j*8u));
        cpu_identification.brand[48]=0;
    }else{
        const char fallback[]="Unknown x86 CPU";
        for(uint32_t i=0u;i<sizeof(fallback);++i)cpu_identification.brand[i]=fallback[i];
    }
    /*
     * Verified SKU fallback: Intel Core i5-12450HX is an 8-core / 12-thread
     * hybrid processor. Do not apply the CPUID topology shift width as a
     * physical-core count. For other CPUs, 0 means the count is unknown.
     */
    if(text_contains(cpu_identification.vendor,"GenuineIntel")&&
       cpu_identification.family==6u&&cpu_identification.model==151u&&
       text_contains(cpu_identification.brand,"i5-12450HX")){
        cpu_identification.physical_cores=8u;
    }else{
        cpu_identification.physical_cores=0u;
    }
    if(cpu_identification.max_extended_leaf>=0x80000001u){
        cpuid_count(0x80000001u,0u,&a,&b,&c,&d);
        cpu_identification.x86_64=(d&(1u<<29))?1u:0u;
    }
    (void)c;(void)d;

    uint32_t acpi_count=acpi_enumerate(multiboot_info,bsp);
    if(acpi_count) {
        cpu_hint=acpi_count;
    } else {
        /*
         * CPUID gives a logical CPU count but not the APIC IDs needed by the
         * current SIPI implementation. Until high-physical ACPI tables are
         * mapped, stay on the BSP instead of dereferencing an invalid APIC ID.
         */
        cpu_hint=1u;
        for(uint32_t i=1u;i<LIONOS_MAX_CPUS;++i) cpus[i].apic_id=0xFFFFFFFFu;
    }
}
uint32_t cpu_count_hint(void){return cpu_hint;}
uint32_t cpu_current_index(void){uint32_t id=lapic_id();if(id!=0xFFFFFFFFu)for(uint32_t i=0;i<LIONOS_MAX_CPUS;++i)if(cpus[i].online&&cpus[i].apic_id==id)return i;return 0u;}
const struct cpu_info*cpu_get(uint32_t index){if(index>=LIONOS_MAX_CPUS)return 0;__sync_synchronize();return &cpus[index];}
void cpu_mark_online(uint32_t index,uint32_t apic_id){if(index>=LIONOS_MAX_CPUS)return;cpus[index].apic_id=apic_id;cpus[index].logical_per_package=1u;__sync_synchronize();cpus[index].online=1u;__sync_synchronize();}


uint32_t cpu_feature_flags(void){ return cpu_features; }
void cpu_get_identification(cpu_identification_t *out){
    if(!out)return;
    *out=cpu_identification;
}

int cpu_fpu_sse_init(void){
    uint32_t flags;
    __asm__ volatile("mov %%cr0,%0":"=r"(flags));
    flags &= ~(1u<<2); /* CR0.EM: hardware floating point is present. */
    flags |=  (1u<<1); /* CR0.MP: WAIT/FWAIT tracks task switching. */
    __asm__ volatile("mov %0,%%cr0"::"r"(flags):"memory");
    if (cpu_features & CPU_FEAT_SSE) {
        __asm__ volatile("mov %%cr4,%0":"=r"(flags));
        flags |= (1u<<9) | (1u<<10); /* OSFXSR + OSXMMEXCPT. */
        __asm__ volatile("mov %0,%%cr4"::"r"(flags):"memory");
        return 0;
    }
    return -1;
}
