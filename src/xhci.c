#include <stdint.h>
#include "io.h"
#include "memory.h"
#include "paging.h"
#include "xhci.h"
#include "debug.h"
#include "console.h"

#define PAGE_SIZE 4096u
#define PCI_ADDR 0xCF8u
#define PCI_DATA 0xCFCu
#define PCI_COMMAND 0x04u
#define PCI_CLASS 0x08u
#define PCI_BAR0 0x10u
#define XHCI_VIRT 0xF1000000u
#define XHCI_MAP_SIZE 0x00100000u

#define CAPLENGTH 0x00u
#define HCSPARAMS1 0x04u
#define HCSPARAMS2 0x08u
#define HCCPARAMS1 0x10u
#define DBOFF 0x14u
#define RTSOFF 0x18u
#define USBCMD 0x00u
#define USBSTS 0x04u
#define PAGESIZE 0x08u
#define CRCR 0x18u
#define DCBAAP 0x30u
#define CONFIG 0x38u
#define PORT_BASE 0x400u
#define PORT_STRIDE 0x10u
#define RUN_STOP 0x1u
#define HCRST 0x2u
#define HCH 0x1u
#define CNR 0x800u
#define PORT_CCS 0x1u
#define PORT_PED 0x2u
#define PORT_PR 0x10u
#define PORT_PP 0x200u
#define PORT_SPEED_MASK 0x3C00u
#define PORT_SPEED_SHIFT 10u
#define PORT_CSC (1u<<17)
#define PORT_PEC (1u<<18)
#define PORT_WRC (1u<<19)
#define PORT_OCC (1u<<20)
#define PORT_PRC (1u<<21)
#define PORT_PLC (1u<<22)
#define PORT_CEC (1u<<23)
#define PORT_CHANGE (PORT_CSC|PORT_PEC|PORT_WRC|PORT_OCC|PORT_PRC|PORT_PLC|PORT_CEC)

#define RT_BASE0 0x20u
#define IMAN 0x00u
#define IMOD 0x04u
#define ERSTSZ 0x08u
#define ERSTBA 0x10u
#define ERDP 0x18u

#define TRB_NORMAL 1u
#define TRB_SETUP 2u
#define TRB_DATA 3u
#define TRB_STATUS 4u
#define TRB_LINK 6u
#define TRB_ENABLE_SLOT 9u
#define TRB_ADDRESS_DEVICE 11u
#define TRB_CONFIGURE_EP 12u
#define TRB_EVAL_CONTEXT 13u
#define TRB_TRANSFER_EVENT 32u
#define TRB_COMMAND_EVENT 33u
#define TRB_PORT_EVENT 34u
#define TRB_CYCLE 1u
#define TRB_TC 2u
#define TRB_CHAIN 0x10u
#define TRB_IOC 0x20u
#define TRB_IDT 0x40u
#define TRB_DIR_IN 0x10000u
#define CC_SUCCESS 1u

#define LEGACY_ID 1u
#define BIOS_OWNED (1u<<16)
#define OS_OWNED (1u<<24)

#define MAX_SLOTS 32u
#define RING_TRBS 256u
#define EVENT_TRBS 256u
#define CONFIG_MAX 4096u
#define MAX_SCRATCH 1024u

typedef struct {
    uint32_t lo, hi, status, control;
} __attribute__((packed, aligned(16))) trb_t;

typedef struct {
    uint64_t base;
    uint32_t size;
    uint32_t reserved;
} __attribute__((packed)) erst_t;

typedef struct {
    uint8_t bus, slot, function;
    uint16_t vendor, device;
    uint64_t bar0;
} pci_xhci_t;

typedef struct {
    uint8_t interface_number;
    uint8_t endpoint_address;
    uint16_t packet_size;
    uint8_t interval;
    uint8_t config_value;
} hid_candidate_t;

static uint32_t cap_len, op_base, db_base, rt_base;
static uint32_t hci_version;
static uint32_t max_ports, max_slots, ctx_size;
static uint32_t slot_id, port_number, device_speed, endpoint_id;
static uint32_t endpoint_packet, endpoint_interval;
static uint32_t ready;

static uint64_t *dcbaa;
static erst_t *erst;
static trb_t *cmd_ring, *event_ring, *ep0_ring, *intr_ring;
static void *out_ctx, *in_ctx;
static uint8_t *control_buf, *config_buf, *report_buf;
static uint64_t *scratch_array;
static void *scratch_pages[MAX_SCRATCH];
static uint32_t scratch_count;

static uint32_t cmd_index, cmd_cycle = 1u;
static uint32_t event_index, event_cycle = 1u;
static uint32_t ep0_index, ep0_cycle = 1u;
static uint32_t intr_index, intr_cycle = 1u;
static uint32_t report_pending;
static uint32_t report_length;

static int usb_fail(const char *stage){
    console_write("[ USB ] xHCI fail: ");
    console_write(stage);
    console_putc('\n');
    debug_write("LIONOS:USB-FAIL-");
    debug_write(stage);
    debug_write("\n");
    return -1;
}

static uint32_t pci_key(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t reg) {
    return 0x80000000u | ((uint32_t)bus<<16) | ((uint32_t)slot<<11) |
           ((uint32_t)fn<<8) | (reg & 0xFCu);
}

static uint32_t pci_r32(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t reg) {
    outl(PCI_ADDR, pci_key(bus,slot,fn,reg));
    return inl(PCI_DATA);
}

static void pci_w32(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t reg, uint32_t v) {
    outl(PCI_ADDR, pci_key(bus,slot,fn,reg));
    outl(PCI_DATA, v);
}

static int find_xhci(pci_xhci_t *out) {
    for (uint32_t b=0;b<256u;++b) for (uint32_t s=0;s<32u;++s) {
        uint32_t id=pci_r32((uint8_t)b,(uint8_t)s,0,0);
        if ((id&0xFFFFu)==0xFFFFu) continue;
        uint32_t hdr=pci_r32((uint8_t)b,(uint8_t)s,0,0x0Cu);
        uint32_t funcs=(hdr&0x00800000u)?8u:1u;
        for(uint32_t f=0;f<funcs;++f){
            uint32_t c=pci_r32((uint8_t)b,(uint8_t)s,(uint8_t)f,PCI_CLASS);
            if((c>>24)!=0x0Cu || ((c>>16)&0xFFu)!=0x03u || ((c>>8)&0xFFu)!=0x30u) continue;
            uint32_t lo=pci_r32((uint8_t)b,(uint8_t)s,(uint8_t)f,PCI_BAR0);
            if(lo&1u) continue;
            uint64_t bar=(uint64_t)(lo&0xFFFFFFF0u);
            if(((lo>>1)&3u)==2u) bar|=(uint64_t)pci_r32((uint8_t)b,(uint8_t)s,(uint8_t)f,PCI_BAR0+4u)<<32;
            if(!bar) continue;
            out->bus=(uint8_t)b; out->slot=(uint8_t)s; out->function=(uint8_t)f;
            out->vendor=(uint16_t)(id&0xFFFFu); out->device=(uint16_t)(id>>16);
            out->bar0=bar;
            return 0;
        }
    }
    return -1;
}

static uint32_t r32(uint32_t off) { return *(volatile uint32_t *)(uintptr_t)(XHCI_VIRT+off); }
static void w32(uint32_t off,uint32_t v) { *(volatile uint32_t *)(uintptr_t)(XHCI_VIRT+off)=v; }
static void w64(uint32_t off,uint64_t v) {
    volatile uint32_t *p=(volatile uint32_t *)(uintptr_t)(XHCI_VIRT+off);
    p[0]=(uint32_t)v; p[1]=(uint32_t)(v>>32);
}

static void zero_mem(void *ptr,uint32_t bytes){
    uint8_t *p=(uint8_t*)ptr;
    for(uint32_t i=0;i<bytes;++i)p[i]=0;
}
static uint64_t xhci_rdtsc(void){
    uint32_t lo,hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo),"=d"(hi) :: "memory");
    return ((uint64_t)hi<<32)|lo;
}

static uint64_t xhci_tsc_hz(void){
    uint32_t a,b,c,d;
    uint64_t hz=0;
    __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(0x15u),"c"(0u));
    if(a&&b&&c) hz=((uint64_t)c*(uint64_t)b)/(uint64_t)a;
    if(!hz){
        __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(0u),"c"(0u));
        uint32_t max_leaf=a;
        if(max_leaf>=0x16u){
            __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(0x16u),"c"(0u));
            if(a) hz=(uint64_t)a*1000000ULL;
        }
    }
    return hz;
}

static void xhci_delay_ms(uint32_t ms){
    uint64_t hz=xhci_tsc_hz();
    if(hz){
        uint64_t start=xhci_rdtsc();
        uint64_t ticks=(hz/1000ULL)*(uint64_t)ms;
        while((xhci_rdtsc()-start)<ticks) __asm__ volatile("pause");
        return;
    }
    /* Fallback for very old/non-reporting virtual CPUs. */
    for(uint32_t m=0;m<ms;++m) for(uint32_t i=0;i<1000u;++i) io_wait();
}

static int map_mmio(uint64_t phys){
    uint64_t aligned=phys&~(uint64_t)(PAGE_SIZE-1u);
    uint32_t offset=(uint32_t)(phys&(PAGE_SIZE-1u));
    uint32_t bytes=(XHCI_MAP_SIZE+offset+PAGE_SIZE-1u)&~(PAGE_SIZE-1u);
    for(uint32_t off=0;off<bytes;off+=PAGE_SIZE)
        if(paging_map_kernel_page(XHCI_VIRT+off,aligned+off,0x13u)!=0)return -1;
    return 0;
}

static int legacy_handoff(void){
    uint32_t ext=((r32(HCCPARAMS1)>>16)&0xFFFFu)*4u;
    for(uint32_t n=0;ext&&n<1024u;++n){
        if(ext >= XHCI_MAP_SIZE) return -1;
        uint32_t v=r32(ext);
        uint32_t next=((v>>8)&0xFFu)*4u;
        if((v&0xFFu)==LEGACY_ID){
            /* Request ownership from firmware before resetting the HC. */
            w32(ext,v|OS_OWNED);
            if(v&BIOS_OWNED){
                for(uint32_t i=0;i<10000000u;++i){
                    if(!(r32(ext)&BIOS_OWNED))break;
                    __asm__ volatile("pause");
                }
                if(r32(ext)&BIOS_OWNED)return -1;
            }
            /* Disable legacy SMI generation once the OS owns the controller. */
            w32(ext+4u,0u);
            return 0;
        }
        ext=next;
    }
    return 0;
}

static int reset_controller(void){
    uint32_t cmd=r32(op_base+USBCMD)&~RUN_STOP;
    w32(op_base+USBCMD,cmd);
    for(uint32_t i=0;i<2000000u;++i){if(r32(op_base+USBSTS)&HCH)break;__asm__ volatile("pause");}
    if(!(r32(op_base+USBSTS)&HCH))return -1;
    w32(op_base+USBCMD,cmd|HCRST);
    /* Linux's Intel xHCI path deliberately waits 1 ms after HCRST before
       touching controller registers. Keep that hardware-safe ordering. */
    xhci_delay_ms(1u);
    for(uint32_t i=0;i<10000000u;++i){if(!(r32(op_base+USBCMD)&HCRST))break;__asm__ volatile("pause");}
    if(r32(op_base+USBCMD)&HCRST)return -1;
    for(uint32_t i=0;i<10000000u;++i){if(!(r32(op_base+USBSTS)&CNR))break;__asm__ volatile("pause");}
    return (r32(op_base+USBSTS)&CNR)?-1:0;
}

static uint32_t scratchpads(uint32_t hcs2){return ((hcs2>>27)&0x1Fu)|(((hcs2>>21)&0x1Fu)<<5);}

static void link_trb(trb_t *ring,uint32_t cycle){
    trb_t *t=&ring[RING_TRBS-1u];
    t->lo=(uint32_t)(uintptr_t)ring; t->hi=0; t->status=0;
    t->control=(TRB_LINK<<10)|TRB_TC|(cycle?TRB_CYCLE:0u);
}

static int dma_page(void **out){
    void *p=page_alloc();
    if(!p)return -1;
    zero_mem(p,PAGE_SIZE);
    if(out)*out=p;
    return 0;
}

static int alloc_memory(void){
    if(dma_page((void**)&dcbaa)||dma_page((void**)&cmd_ring)||dma_page((void**)&event_ring)||
       dma_page((void**)&erst)||dma_page((void**)&ep0_ring)||dma_page((void**)&intr_ring)||
       dma_page(&out_ctx)||dma_page(&in_ctx)||dma_page((void**)&control_buf)||
       dma_page((void**)&config_buf)||dma_page((void**)&report_buf)) return -1;

    scratch_count=scratchpads(r32(HCSPARAMS2));
    if(scratch_count>MAX_SCRATCH)return -1;
    if(scratch_count){
        scratch_array=(uint64_t*)page_alloc();
        if(!scratch_array)return -1;
        zero_mem(scratch_array,PAGE_SIZE);
        for(uint32_t i=0;i<scratch_count;++i){
            scratch_pages[i]=page_alloc();
            if(!scratch_pages[i])return -1;
            zero_mem(scratch_pages[i],PAGE_SIZE);
            scratch_array[i]=(uint64_t)(uintptr_t)scratch_pages[i];
        }
        dcbaa[0]=(uint64_t)(uintptr_t)scratch_array;
    }
    link_trb(cmd_ring,1u); link_trb(ep0_ring,1u); link_trb(intr_ring,1u);
    erst[0].base=(uint64_t)(uintptr_t)event_ring;
    erst[0].size=EVENT_TRBS;
    return 0;
}

static void init_rings(void){
    cmd_index=0;cmd_cycle=1;event_index=0;event_cycle=1;ep0_index=0;ep0_cycle=1;intr_index=0;intr_cycle=1;
    w64(op_base+CRCR,(uint64_t)(uintptr_t)cmd_ring|1u);
    uint32_t ir=rt_base+RT_BASE0;
    w32(ir+IMAN,0u);w32(ir+IMOD,0u);w32(ir+ERSTSZ,1u);w64(ir+ERSTBA,(uint64_t)(uintptr_t)erst);w64(ir+ERDP,(uint64_t)(uintptr_t)event_ring);
}

static int run_controller(void){
    uint32_t slots=max_slots; if(slots>255u)slots=255u;if(!slots)slots=1u;
    w32(op_base+CONFIG,slots);
    w64(op_base+DCBAAP,(uint64_t)(uintptr_t)dcbaa);
    w32(op_base+USBCMD,r32(op_base+USBCMD)|RUN_STOP);
    for(uint32_t i=0;i<2000000u;++i){if(!(r32(op_base+USBSTS)&HCH))return 0;__asm__ volatile("pause");}
    return -1;
}

static void submit_cmd(uint32_t type,uint64_t param,uint32_t control){
    trb_t *t=&cmd_ring[cmd_index];
    t->lo=(uint32_t)param;t->hi=(uint32_t)(param>>32);t->status=0;t->control=(type<<10)|control|(cmd_cycle?TRB_CYCLE:0u);
    ++cmd_index;
    if(cmd_index>=RING_TRBS-1u){link_trb(cmd_ring,cmd_cycle);cmd_index=0;cmd_cycle^=1u;}
    __asm__ volatile("mfence" ::: "memory");
    *(volatile uint32_t *)(uintptr_t)(XHCI_VIRT+db_base)=0u;
}

static int next_event(trb_t *out){
    trb_t *t=&event_ring[event_index];
    if((t->control&TRB_CYCLE)!=(event_cycle?TRB_CYCLE:0u))return -1;
    if(out)*out=*t;
    t->control=0u;
    ++event_index;
    if(event_index>=EVENT_TRBS){event_index=0;event_cycle^=1u;}
    w64(rt_base+RT_BASE0+ERDP,(uint64_t)(uintptr_t)&event_ring[event_index]|8u);
    return 0;
}

static int wait_cmd(uint32_t want_slot){
    for(uint32_t n=0;n<5000000u;++n){
        trb_t e;if(next_event(&e)!=0){__asm__ volatile("pause");continue;}
        uint32_t type=(e.control>>10)&0x3Fu;
        if(type==TRB_PORT_EVENT)continue;
        if(type!=TRB_COMMAND_EVENT)continue;
        if(want_slot&&((e.control>>24)&0xFFu)!=want_slot)continue;
        return (((e.status>>24)&0xFFu))==CC_SUCCESS?0:-1;
    }
    return -1;
}

static int enable_slot(void){
    submit_cmd(TRB_ENABLE_SLOT,0,0);
    for(uint32_t n=0;n<5000000u;++n){
        trb_t e;if(next_event(&e)!=0){__asm__ volatile("pause");continue;}
        uint32_t type=(e.control>>10)&0x3Fu;if(type==TRB_PORT_EVENT)continue;if(type!=TRB_COMMAND_EVENT)continue;
        uint32_t cc=(e.status>>24)&0xFFu;
        if(cc!=CC_SUCCESS)return -1;
        slot_id=(e.control>>24)&0xFFu;
        return slot_id?0:-1;
    }
    return -1;
}

static uint8_t *ctx_slot(void *ctx){return (uint8_t*)ctx+ctx_size;}
static uint8_t *ctx_ep(void *ctx,uint32_t dci){return (uint8_t*)ctx+ctx_size*(dci+1u);}
static uint64_t qget(void *ctx,uint32_t dword){uint32_t *p=(uint32_t*)ctx;return (uint64_t)p[dword]|((uint64_t)p[dword+1u]<<32);}
static void qset(void *ctx,uint32_t dword,uint64_t v){uint32_t *p=(uint32_t*)ctx;p[dword]=(uint32_t)v;p[dword+1u]=(uint32_t)(v>>32);}

static uint32_t ep0_default_mps(void){if(device_speed==3u)return 64u;if(device_speed>=4u)return 512u;return 8u;}

static void fill_slot(void *ctx,uint32_t entries){
    uint32_t *s=(uint32_t*)ctx_slot(ctx);zero_mem(s,ctx_size);
    s[0]=((device_speed&0xFu)<<20)|((entries&0x1Fu)<<27);
    s[1]=(port_number&0xFFu)<<16;
}

static void fill_ep0(void *ctx,uint32_t mps,uint64_t dequeue){
    uint32_t *e=(uint32_t*)ctx_ep(ctx,1u);zero_mem(e,ctx_size);
    e[0]=0u;
    e[1]=(3u<<1)|(4u<<3)|((mps&0xFFFFu)<<16);
    qset(e,2u,dequeue|(ep0_cycle?1u:0u));
    e[4]=(hci_version>=0x100u)?8u:(mps&0xFFFFu);
}

static int address_device(void){
    zero_mem(in_ctx,PAGE_SIZE);dcbaa[slot_id]=(uint64_t)(uintptr_t)out_ctx;((uint32_t*)in_ctx)[1]=3u;
    fill_slot(in_ctx,1u);fill_ep0(in_ctx,ep0_default_mps(),(uint64_t)(uintptr_t)ep0_ring);
    submit_cmd(TRB_ADDRESS_DEVICE,(uint64_t)(uintptr_t)in_ctx,slot_id<<24);
    return wait_cmd(slot_id);
}

static int update_ep0_mps(uint32_t mps){
    uint8_t *out_ep=ctx_ep(out_ctx,1u);
    uint64_t dequeue=qget(out_ep,2u);
    zero_mem(in_ctx,PAGE_SIZE);((uint32_t*)in_ctx)[1]=(1u<<1);fill_ep0(in_ctx,mps,dequeue&~1ull);
    submit_cmd(TRB_EVAL_CONTEXT,(uint64_t)(uintptr_t)in_ctx,slot_id<<24);
    return wait_cmd(slot_id);
}

static void write_trb(trb_t *t,uint64_t param,uint32_t status,uint32_t type,uint32_t ctl,uint32_t cycle){
    t->lo=(uint32_t)param;t->hi=(uint32_t)(param>>32);t->status=status;t->control=(type<<10)|ctl|(cycle?TRB_CYCLE:0u);
}

static void advance_ep0(void){
    ++ep0_index;if(ep0_index>=RING_TRBS-1u){link_trb(ep0_ring,ep0_cycle);ep0_index=0;ep0_cycle^=1u;}
}

static int control_xfer(uint8_t bm,uint8_t req,uint16_t value,uint16_t index,void *data,uint16_t length,int in){
    uint64_t setup=(uint64_t)bm|((uint64_t)req<<8)|((uint64_t)value<<16)|((uint64_t)index<<32)|((uint64_t)length<<48);
    uint32_t trt=length?(in?(3u<<16):(2u<<16)):0u;
    write_trb(&ep0_ring[ep0_index],setup,8u,TRB_SETUP,TRB_IDT|TRB_CHAIN|trt,ep0_cycle);advance_ep0();
    if(length){write_trb(&ep0_ring[ep0_index],(uint64_t)(uintptr_t)data,length&0x1FFFFu,TRB_DATA,TRB_CHAIN|(in?TRB_DIR_IN:0u),ep0_cycle);advance_ep0();}
    write_trb(&ep0_ring[ep0_index],0,0,TRB_STATUS,TRB_IOC|(in?0u:TRB_DIR_IN),ep0_cycle);advance_ep0();
    __asm__ volatile("mfence" ::: "memory");
    *(volatile uint32_t *)(uintptr_t)(XHCI_VIRT+db_base+slot_id*4u)=1u;
    for(uint32_t n=0;n<5000000u;++n){
        trb_t e;if(next_event(&e)!=0){__asm__ volatile("pause");continue;}
        if(((e.control>>10)&0x3Fu)!=TRB_TRANSFER_EVENT)continue;
        if(((e.control>>24)&0xFFu)!=slot_id||((e.control>>16)&0x1Fu)!=1u)continue;
        return (((e.status>>24)&0xFFu))==CC_SUCCESS?0:-1;
    }
    return -1;
}

static int get_device(void){
    zero_mem(control_buf,PAGE_SIZE);
    if(control_xfer(0x80u,6u,0x0100u,0,control_buf,8u,1))return -1;
    if(control_buf[0]<8u||control_buf[1]!=1u)return -1;
    uint32_t mps=control_buf[7];if(device_speed>=4u)mps=512u;
    if(mps!=ep0_default_mps()&&update_ep0_mps(mps))return -1;
    ep0_index=3u;ep0_cycle=1u;
    zero_mem(control_buf,PAGE_SIZE);
    if(control_xfer(0x80u,6u,0x0100u,0,control_buf,18u,1))return -1;
    return (control_buf[0]>=18u&&control_buf[1]==1u)?0:-1;
}

static int find_hid(hid_candidate_t *c){
    zero_mem(config_buf,PAGE_SIZE);
    if(control_xfer(0x80u,6u,0x0200u,0,config_buf,9u,1))return -1;
    if(config_buf[1]!=2u||config_buf[0]<9u)return -1;
    uint16_t total=(uint16_t)config_buf[2]|((uint16_t)config_buf[3]<<8);if(total<9u||total>CONFIG_MAX)return -1;
    zero_mem(config_buf,PAGE_SIZE);if(control_xfer(0x80u,6u,0x0200u,0,config_buf,total,1))return -1;
    zero_mem(c,sizeof(*c));c->config_value=config_buf[5];
    uint32_t i=0;int selected=0;
    while(i+2u<=total){uint8_t len=config_buf[i],type=config_buf[i+1u];if(len<2u||i+len>total)return -1;
        if(type==4u&&len>=9u){
            uint8_t alt=config_buf[i+3u];
            uint8_t cls=config_buf[i+5u],sub=config_buf[i+6u],proto=config_buf[i+7u];
            selected=(alt==0u&&cls==3u&&sub==1u&&proto==2u);
            if(selected)c->interface_number=config_buf[i+2u];
        }else if(type==5u&&len>=7u&&selected){
            uint8_t addr=config_buf[i+2u],attr=config_buf[i+3u];uint16_t mps=(uint16_t)config_buf[i+4u]|((uint16_t)config_buf[i+5u]<<8);
            if((addr&0x80u)&&(attr&3u)==3u&&(mps&0x7FFu)){c->endpoint_address=addr;c->packet_size=mps&0x7FFu;c->interval=config_buf[i+6u]?config_buf[i+6u]:1u;return 0;}
        }
        i+=len;
    }
    return -1;
}

static uint32_t interval_value(uint32_t v){
    if(!v)v=1u;
    if(device_speed>=3u){
        if(v>16u)v=16u;
        return v-1u;
    }
    uint32_t microframes=v*8u;
    if(microframes<8u)microframes=8u;
    if(microframes>1024u)microframes=1024u;
    uint32_t e=3u;
    while(e<10u && (1u<<(e+1u))<=microframes)++e;
    return e;
}

static void fill_intr_ep(void *ctx){
    uint32_t *e=(uint32_t*)ctx_ep(ctx,endpoint_id);zero_mem(e,ctx_size);
    e[0]=(endpoint_interval&0xFFu)<<16;
    e[1]=(3u<<1)|(7u<<3)|((endpoint_packet&0xFFFFu)<<16);
    qset(e,2u,(uint64_t)(uintptr_t)intr_ring|(intr_cycle?1u:0u));
    e[4]=endpoint_packet&0xFFFFu;
}

static int configure_mouse(hid_candidate_t *c){
    uint32_t n=c->endpoint_address&0x0Fu;if(!n||(c->endpoint_address&0x80u)==0)return -1;
    endpoint_id=n*2u+1u;if(endpoint_id>=32u)return -1;
    endpoint_packet=c->packet_size;if(endpoint_packet>1024u)endpoint_packet=1024u;endpoint_interval=interval_value(c->interval);
    zero_mem(in_ctx,PAGE_SIZE);((uint32_t*)in_ctx)[1]=1u|(1u<<endpoint_id);fill_slot(in_ctx,endpoint_id);fill_intr_ep(in_ctx);
    submit_cmd(TRB_CONFIGURE_EP,(uint64_t)(uintptr_t)in_ctx,slot_id<<24);return wait_cmd(slot_id);
}

static int set_protocol(uint8_t iface){
    if(control_xfer(0x21u,0x0Bu,0u,iface,0,0,0))return -1;
    (void)control_xfer(0x21u,0x0Au,0u,iface,0,0,0);
    return 0;
}

static void advance_intr(void){
    ++intr_index;if(intr_index>=RING_TRBS-1u){link_trb(intr_ring,intr_cycle);intr_index=0;intr_cycle^=1u;}
}

static int submit_report(void){
    if(report_pending)return 0;
    report_length=endpoint_packet;
    if(report_length<3u)report_length=3u;
    if(report_length>PAGE_SIZE)report_length=PAGE_SIZE;
    write_trb(&intr_ring[intr_index],(uint64_t)(uintptr_t)report_buf,report_length&0x1FFFFu,TRB_NORMAL,TRB_IOC,intr_cycle);advance_intr();
    __asm__ volatile("mfence" ::: "memory");
    *(volatile uint32_t *)(uintptr_t)(XHCI_VIRT+db_base+slot_id*4u)=endpoint_id;
    report_pending=1u;return 0;
}

int xhci_mouse_init(void){
    if(ready)return 0;
    pci_xhci_t d;if(find_xhci(&d))return usb_fail("PCI");
    console_write("[ USB ] xHCI controller ");console_write_hex(d.vendor);console_putc(':');console_write_hex(d.device);console_putc('\n');
    uint32_t pcicmd=pci_r32(d.bus,d.slot,d.function,PCI_COMMAND);pcicmd|=0x6u;pci_w32(d.bus,d.slot,d.function,PCI_COMMAND,pcicmd);
    if(map_mmio(d.bar0))return usb_fail("MMIO");
    cap_len=r32(CAPLENGTH)&0xFFu;hci_version=(r32(CAPLENGTH)>>16)&0xFFFFu;
    if(cap_len<0x20u)return usb_fail("CAP");
    op_base=cap_len;db_base=r32(DBOFF)&~3u;rt_base=r32(RTSOFF)&~0x1Fu;
    uint32_t hcs1=r32(HCSPARAMS1);max_slots=hcs1&0xFFu;max_ports=(hcs1>>24)&0xFFu;ctx_size=(r32(HCCPARAMS1)&4u)?64u:32u;
    if(!max_slots||!max_ports||(r32(op_base+PAGESIZE)&1u)==0)return usb_fail("PARAMS");
    console_write("[ USB ] xHCI version ");console_write_hex(hci_version);
    console_write(" / context ");console_write_dec(ctx_size);
    console_write(" / ports ");console_write_dec(max_ports);console_write("\\n");
    if(legacy_handoff())return usb_fail("LEGACY");
    if(reset_controller())return usb_fail("RESET");
    if(alloc_memory())return usb_fail("ALLOC");
    init_rings();if(run_controller())return usb_fail("RUN");
    uint32_t saw_connected=0u;
    uint32_t saw_reset_timeout=0u;
    for(uint32_t p=1;p<=max_ports;++p){
        uint32_t po=op_base+PORT_BASE+(p-1u)*PORT_STRIDE,ps=r32(po);if(!(ps&PORT_CCS))continue;
        saw_connected=1u;
        w32(po,(ps&~PORT_CHANGE)|PORT_PP|PORT_PR);
        /* USB hub reset recovery: give the device at least 10 ms before
           interpreting PORTSC and starting enumeration. */
        xhci_delay_ms(10u);
        int reset=0;for(uint32_t n=0;n<10000000u;++n){uint32_t q=r32(po);if(!(q&PORT_PR)&&(q&PORT_CCS)){reset=1;break;}__asm__ volatile("pause");}
        if(!reset){saw_reset_timeout=1u;continue;}
        ps=r32(po);
        if(!(ps&PORT_CCS)||!(ps&PORT_PED)) continue;
        w32(po,(ps&~PORT_CHANGE)|PORT_PP);
        ps=r32(po);
        port_number=p;device_speed=(ps&PORT_SPEED_MASK)>>PORT_SPEED_SHIFT;
        if(!device_speed){console_write("[ USB ] xHCI fail: SPEED\\n");debug_write("LIONOS:USB-FAIL-SPEED\\n");continue;}
        if(enable_slot()){console_write("[ USB ] xHCI fail: ENABLE-SLOT\\n");debug_write("LIONOS:USB-FAIL-ENABLE-SLOT\\n");continue;}
        if(address_device()){console_write("[ USB ] xHCI fail: ADDRESS\\n");debug_write("LIONOS:USB-FAIL-ADDRESS\\n");continue;}
        if(get_device()){console_write("[ USB ] xHCI fail: DESCRIPTOR\\n");debug_write("LIONOS:USB-FAIL-DESCRIPTOR\\n");continue;}
        hid_candidate_t c;if(find_hid(&c)||!c.config_value){console_write("[ USB ] xHCI fail: HID\\n");debug_write("LIONOS:USB-FAIL-HID\\n");continue;}
        /* USB devices transition to Configured state before non-EP0 endpoints are enabled. */
        if(control_xfer(0,9u,c.config_value,0,0,0,0)){console_write("[ USB ] xHCI fail: SET-CONFIG\\n");debug_write("LIONOS:USB-FAIL-SET-CONFIG\\n");continue;}
        if(configure_mouse(&c)){console_write("[ USB ] xHCI fail: CONFIGURE-EP\\n");debug_write("LIONOS:USB-FAIL-CONFIGURE-EP\\n");continue;}
        if(set_protocol(c.interface_number)){console_write("[ USB ] xHCI fail: SET-PROTOCOL\\n");debug_write("LIONOS:USB-FAIL-SET-PROTOCOL\\n");continue;}
        endpoint_packet=c.packet_size;if(endpoint_packet>PAGE_SIZE)endpoint_packet=PAGE_SIZE;
        console_write("[ USB ] HID mouse interface ");console_write_dec(c.interface_number);
        console_write(" endpoint ");console_write_hex(c.endpoint_address);
        console_write(" packet ");console_write_dec(endpoint_packet);
        console_write(" interval ");console_write_dec(c.interval);console_write("\\n");
        report_pending=0;report_length=0;
        if(submit_report()){console_write("[ USB ] xHCI fail: REPORT\\n");debug_write("LIONOS:USB-FAIL-REPORT\\n");continue;}
        ready=1u;debug_write("LIONOS:USB-MOUSE-READY\n");console_write("[ OK ] xHCI HID mouse ready on root port ");console_write_dec(port_number);console_putc('\n');return 0;
    }
    if(saw_reset_timeout)return usb_fail("PORT-RESET");
    if(!saw_connected)return usb_fail("NO-PORT");
    return usb_fail("NO-HID-MOUSE");
}

int xhci_mouse_poll(int32_t *dx,int32_t *dy,uint8_t *buttons){
    if(dx) *dx=0;
    if(dy) *dy=0;
    if(buttons) *buttons=0;
    if(!ready)return 0;
    trb_t e;
    while(next_event(&e)==0){
        if(((e.control>>10)&0x3Fu)!=TRB_TRANSFER_EVENT)continue;
        if(((e.control>>24)&0xFFu)!=slot_id||((e.control>>16)&0x1Fu)!=endpoint_id)continue;
        report_pending=0;
        uint32_t cc=((e.status>>24)&0xFFu);
        if(cc==CC_SUCCESS||cc==13u){
            if(report_length>=3u){
                if(buttons)*buttons=report_buf[0]&7u;
                if(dx)*dx=(int32_t)(int8_t)report_buf[1];
                if(dy)*dy=-(int32_t)(int8_t)report_buf[2];
            }
            (void)submit_report();
            return 1;
        }
        (void)submit_report();
        return -1;
    }
    return 0;
}
