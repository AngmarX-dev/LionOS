#include <stdint.h>
#include "cpu.h"
#include "lapic.h"

#define MB2_TAG_END 0u
#define MB2_TAG_ACPI_OLD 14u
#define MB2_TAG_ACPI_NEW 15u
#define ACPI_MAX_CPUS LIONOS_MAX_CPUS
#define KERNEL_IDENTITY_LIMIT 0x10000000u

struct mb2_tag { uint32_t type,size; };
struct acpi_rsdp20 { char sig[8]; uint8_t checksum; char oem[6]; uint8_t revision; uint32_t rsdt; uint32_t length; uint64_t xsdt; uint8_t ext_checksum; uint8_t reserved[3]; } __attribute__((packed));
struct acpi_sdt { char sig[4]; uint32_t length; uint8_t revision,checksum; char oem_id[6]; char oem_table[8]; uint32_t oem_revision,creator_id,creator_revision; } __attribute__((packed));
struct acpi_madt { struct acpi_sdt h; uint32_t lapic_address,flags; } __attribute__((packed));

static struct cpu_info cpus[LIONOS_MAX_CPUS];
static uint32_t cpu_hint=1u;

static uint8_t checksum8(const uint8_t*p,uint32_t n){uint8_t s=0;for(uint32_t i=0;i<n;++i)s=(uint8_t)(s+p[i]);return s;}
static int sig4(const char*p,const char*s){return p[0]==s[0]&&p[1]==s[1]&&p[2]==s[2]&&p[3]==s[3];}
static int identity_range_ok(uint32_t address,uint32_t length){
    if(!address || address >= KERNEL_IDENTITY_LIMIT) return 0;
    if(length > KERNEL_IDENTITY_LIMIT - address) return 0;
    return 1;
}
static void reset_cpus(void){for(uint32_t i=0;i<LIONOS_MAX_CPUS;++i){cpus[i].index=i;cpus[i].apic_id=0xFFFFFFFFu;cpus[i].logical_per_package=1u;cpus[i].online=0u;}}
static uint32_t acpi_find_madt(uint32_t rsdt,uint64_t xsdt,uint8_t revision){
    /*
     * The kernel currently identity-maps only the low 256 MiB. Firmware may
     * legally place RSDT/XSDT/MADT above that range. Never dereference an
     * unmapped physical ACPI address: doing so raises a kernel page fault.
     * A future ACPI mapper can remove this conservative guard.
     */
    if(revision>=2u && (uint32_t)(xsdt>>32)==0u && xsdt){
        uint32_t address=(uint32_t)xsdt;
        if(!identity_range_ok(address,36u)) return 0u;
        const struct acpi_sdt*h=(const struct acpi_sdt*)(uintptr_t)address;
        if(sig4(h->sig,"XSDT")&&h->length>=36u&&identity_range_ok(address,h->length)&&checksum8((const uint8_t*)h,h->length)==0u){
            uint32_t count=(h->length-36u)/8u;const uint8_t*p=(const uint8_t*)h+36u;
            for(uint32_t i=0;i<count;++i){
                uint64_t a=*(const uint64_t*)(const void*)(p+i*8u);
                if((uint32_t)(a>>32))continue;
                uint32_t table=(uint32_t)a;
                if(!identity_range_ok(table,36u))continue;
                const struct acpi_sdt*t=(const struct acpi_sdt*)(uintptr_t)table;
                if(t->length>=36u&&identity_range_ok(table,t->length)&&sig4(t->sig,"APIC")&&checksum8((const uint8_t*)t,t->length)==0u)return table;
            }
        }
    }
    if(rsdt){
        if(!identity_range_ok(rsdt,36u)) return 0u;
        const struct acpi_sdt*h=(const struct acpi_sdt*)(uintptr_t)rsdt;
        if(sig4(h->sig,"RSDT")&&h->length>=36u&&identity_range_ok(rsdt,h->length)&&checksum8((const uint8_t*)h,h->length)==0u){
            uint32_t count=(h->length-36u)/4u;const uint8_t*p=(const uint8_t*)h+36u;
            for(uint32_t i=0;i<count;++i){
                uint32_t a=*(const uint32_t*)(const void*)(p+i*4u);
                if(!identity_range_ok(a,36u))continue;
                const struct acpi_sdt*t=(const struct acpi_sdt*)(uintptr_t)a;
                if(t->length>=36u&&identity_range_ok(a,t->length)&&sig4(t->sig,"APIC")&&checksum8((const uint8_t*)t,t->length)==0u)return a;
            }
        }
    }
    return 0;
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
    if(!identity_range_ok(madt,m->h.length)||m->h.length<44u)return 0u;

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
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0u),"c"(0u));
    uint32_t max_leaf=a;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1u),"c"(0u));
    uint32_t logical=(b>>16)&0xFFu;if(!logical)logical=1u;if(logical>LIONOS_MAX_CPUS)logical=LIONOS_MAX_CPUS;
    uint32_t bsp=(b>>24)&0xFFu;
    cpu_hint=logical;cpus[0].apic_id=bsp;cpus[0].logical_per_package=logical;cpus[0].online=1u;
    if(max_leaf>=0xBu){__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0xBu),"c"(0u));if(b&&b<=LIONOS_MAX_CPUS)cpu_hint=b;}
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
