/*
 * xhci.c — xHCI host controller driver with HID mouse support
 * LionOS — experimental operating system
 *
 * Supports USB 1.x / 2.0 / 3.x / 3.1 / 3.2.
 *
 * Reference: Linux drivers/usb/host/xhci.c, xhci-pci.c, xhci-ring.c
 *
 * Key points:
 *   - BSR=1 in Address Device is a SuperSpeed-only feature.
 *   - Intel controllers expose a VSEC with a USB3_PORTS register that
 *     must be written to enable SuperSpeed and SuperSpeedPlus ports.
 *   - USB 2 ports must reach CCS+PED+!PR before any transfer.
 *   - EP0 Average TRB Length must be 8 (xHCI 1.2 §6.2.3).
 */

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

#define CAP_CAPLENGTH 0x00u
#define CAP_HCSPARAMS1 0x04u
#define CAP_HCSPARAMS2 0x08u
#define CAP_HCCPARAMS1 0x10u
#define CAP_DBOFF 0x14u
#define CAP_RTSOFF 0x18u

#define OP_USBCMD 0x00u
#define OP_USBSTS 0x04u
#define OP_PAGESIZE 0x08u
#define OP_CRCR 0x18u
#define OP_DCBAAP 0x30u
#define OP_CONFIG 0x38u
#define OP_PORT_BASE 0x400u
#define OP_PORT_STRIDE 0x10u

#define CMD_RUN 0x1u
#define CMD_HCRST 0x2u
#define CMD_INTE 0x4u
#define STS_HCH 0x1u
#define STS_CNR 0x800u

#define PS_CCS 0x1u
#define PS_PED 0x2u
#define PS_OCA 0x8u
#define PS_PR 0x10u
#define PS_PLS_MASK (0xFu << 5)
#define PS_PLS_U0 (0u << 5)
#define PS_PP 0x200u
#define PS_SPEED_MASK 0x3C00u
#define PS_SPEED_SHIFT 10u
#define PS_CSC (1u << 17)
#define PS_PEC (1u << 18)
#define PS_WRC (1u << 19)
#define PS_OCC (1u << 20)
#define PS_PRC (1u << 21)
#define PS_PLC (1u << 22)
#define PS_CEC (1u << 23)
#define PS_WPR (1u << 31)
#define PS_LWS (1u << 16)
#define PS_CHANGE_BITS (PS_CSC|PS_PEC|PS_WRC|PS_OCC|PS_PRC|PS_PLC|PS_CEC)
/*
 * PORTSC fields that are safe to carry from a read into a write.
 * Linux calls this "port state to neutral": preserve only RW state
 * fields; RW1S/RW1C/reserved fields must not be replayed.
 */
#define PS_INDICATOR_MASK (3u << 14)
#define PS_WAKE_MASK (7u << 25)
#define PS_RWS_BITS (PS_PLS_MASK|PS_PP|PS_INDICATOR_MASK|PS_WAKE_MASK)
static uint32_t port_state_neutral(uint32_t ps){
    return ps & PS_RWS_BITS;
}

#define RT_IR0 0x20u
#define IR_IMAN 0x00u
#define IR_IMOD 0x04u
#define IR_ERSTSZ 0x08u
#define IR_ERSTBA 0x10u
#define IR_ERDP 0x18u

#define TRB_NORMAL 1u
#define TRB_SETUP 2u
#define TRB_DATA 3u
#define TRB_STATUS 4u
#define TRB_LINK 6u
#define TRB_ENABLE_SLOT 9u
#define TRB_ADDRESS_DEVICE 11u
#define TRB_CONFIGURE_EP 12u
#define TRB_EVAL_CONTEXT 13u
#define TRB_RESET_EP 14u
#define TRB_SET_DEQ 16u
#define TRB_NOOP_CMD 23u
#define TRB_TRANSFER_EVT 32u
#define TRB_CMD_EVT 33u
#define TRB_PORT_EVT 34u
#define TRB_BW_EVT 35u

#define TRB_CYCLE 0x1u
#define TRB_TC 0x2u
#define TRB_ISP 0x4u
#define TRB_CHAIN 0x10u
#define TRB_IOC 0x20u
#define TRB_IDT 0x40u
#define TRB_BSR 0x200u
#define TRB_DIR_IN 0x10000u

#define CC_SUCCESS 1u
#define CC_SHORT_PKT 13u
#define CC_RING_FULL 21u
#define CC_CMD_STOPPED 24u

#define EXT_USBLEGSUP_ID 1u
#define USBLEGSUP_BIOS_OWNED (1u << 16)
#define USBLEGSUP_OS_OWNED (1u << 24)

/* xHCI spec extended capability IDs */
#define EXT_USB_PROTOCOL_ID 2u
#define EXT_USB_LEGSUP_ID 1u
#define EXT_VENDOR_ID 0x0Cu     /* Intel VSEC uses this extended cap ID */
#define INTEL_VSEC_ID 1u        /* Intel VSEC revision/vendor tag */
#define INTEL_VSEC_USB3_PORTS 0x0Au  /* offset of USB3_PORTS register in VSEC */

#define PROTO_USB2 1u
#define PROTO_USB3 2u

#define SPEED_UNDEF 0u
#define SPEED_FULL 1u
#define SPEED_LOW 2u
#define SPEED_HIGH 3u
#define SPEED_SUPER 4u
#define SPEED_SUPER_PLUS 5u

#define RING_TRBS 256u
#define EVENT_TRBS 256u
#define INTR_SEGMENTS 4u
#define MAX_SCRATCH 1024u
#define MAX_PORTS 256u
#define MAX_PROTOCOLS 8u

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
} pci_dev_t;

typedef struct {
    uint8_t major;
    uint8_t minor;
    uint8_t protocol;
    uint8_t port_count;
    uint8_t port_offset;
    uint8_t slot_type;
} usb_protocol_t;

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
static uint32_t ep0_mps;
static uint32_t ready;
static uint32_t ppc_enabled;
static uint16_t xhci_vendor_id;   /* PCI vendor of the xHCI controller */

static uint64_t *dcbaa;
static erst_t *erst;
static trb_t *cmd_ring, *event_ring, *ep0_ring, *intr_ring;
static trb_t *intr_segments[INTR_SEGMENTS];
static void *out_ctx, *in_ctx;
static uint8_t *control_buf, *config_buf, *report_buf;
static uint64_t *scratch_array;
static void *scratch_pages[MAX_SCRATCH];
static uint32_t scratch_count;

static uint32_t cmd_index, cmd_cycle;
static uint32_t event_index, event_cycle;
static uint32_t ep0_index, ep0_cycle;
static uint32_t intr_index, intr_cycle, intr_segment;
static uint32_t report_pending, report_length, report_seen;

static usb_protocol_t protocols[MAX_PROTOCOLS];
static uint32_t protocol_count;

static const char *diag_stage = "NOT STARTED";
static uint32_t diag_controller_found, diag_init_ok;
static uint32_t diag_transfer_submitted, diag_event_count, diag_success_count;
static uint32_t diag_error_count, diag_report_count;
static uint32_t diag_last_cc, diag_last_portsc, diag_last_report_len;
static uint8_t  diag_last_report[8];
static uint16_t diag_vid, diag_pid;
static uint8_t  diag_device_class, diag_device_subclass, diag_device_protocol;
static uint8_t  diag_hid_iface, diag_hid_subclass, diag_hid_protocol;
static uint8_t  diag_hid_endpoint, diag_hid_ep_type;
static uint16_t diag_hid_packet;
static uint8_t  diag_hid_interval;
static uint32_t diag_interface_count, diag_endpoint_count, diag_hid_count;

static void usb_log(const char *s) { console_write(s); debug_write(s); }
static void usb_log_hex(uint32_t v) { console_write_hex(v); }
static void usb_log_dec(uint32_t v) { console_write_dec(v); }
static void usb_log_nl(void) { console_putc('\n'); debug_write("\n"); }

static int usb_fail(const char *stage){
    diag_stage = stage;
    diag_init_ok = 0;
    usb_log("[ USB ] xHCI fail: "); usb_log(stage); usb_log_nl();
    return -1;
}

static void zero_mem(void *p, uint32_t n){
    uint8_t *b = (uint8_t*)p;
    for(uint32_t i=0;i<n;++i) b[i]=0;
}
static void xhci_delay_ms(uint32_t ms){
    for(uint32_t m=0;m<ms;++m)
        for(uint32_t i=0;i<1000u;++i)
            io_wait();
}
#define dma_wmb() __asm__ volatile("mfence" ::: "memory")

static uint32_t pci_key(uint8_t b,uint8_t s,uint8_t f,uint8_t r){
    return 0x80000000u|((uint32_t)b<<16)|((uint32_t)s<<11)|((uint32_t)f<<8)|(r&0xFCu);
}
static uint32_t pci_r32(uint8_t b,uint8_t s,uint8_t f,uint8_t r){
    outl(PCI_ADDR,pci_key(b,s,f,r)); return inl(PCI_DATA);
}
static void pci_w32(uint8_t b,uint8_t s,uint8_t f,uint8_t r,uint32_t v){
    outl(PCI_ADDR,pci_key(b,s,f,r)); outl(PCI_DATA,v);
}
static int pci_find_xhci(pci_dev_t *out){
    for(uint32_t b=0;b<256u;++b) for(uint32_t s=0;s<32u;++s){
        uint32_t id=pci_r32((uint8_t)b,(uint8_t)s,0,0);
        if((id&0xFFFFu)==0xFFFFu) continue;
        uint32_t hdr=pci_r32((uint8_t)b,(uint8_t)s,0,0x0Cu);
        uint32_t funcs=(hdr&0x00800000u)?8u:1u;
        for(uint32_t f=0;f<funcs;++f){
            uint32_t c=pci_r32((uint8_t)b,(uint8_t)s,(uint8_t)f,PCI_CLASS);
            if((c>>24)!=0x0Cu||((c>>16)&0xFFu)!=0x03u||((c>>8)&0xFFu)!=0x30u) continue;
            uint32_t lo=pci_r32((uint8_t)b,(uint8_t)s,(uint8_t)f,PCI_BAR0);
            if(lo&1u) continue;
            uint64_t bar=(uint64_t)(lo&0xFFFFFFF0u);
            if(((lo>>1)&3u)==2u) bar|=(uint64_t)pci_r32((uint8_t)b,(uint8_t)s,(uint8_t)f,PCI_BAR0+4u)<<32;
            if(!bar) continue;
            out->bus=(uint8_t)b; out->slot=(uint8_t)s; out->function=(uint8_t)f;
            out->vendor=(uint16_t)(id&0xFFFFu); out->device=(uint16_t)(id>>16);
            out->bar0=bar; return 0;
        }
    }
    return -1;
}

static uint32_t r32(uint32_t o){ return *(volatile uint32_t *)(uintptr_t)(XHCI_VIRT+o); }
static void w32(uint32_t o,uint32_t v){ *(volatile uint32_t *)(uintptr_t)(XHCI_VIRT+o)=v; }
static void w64(uint32_t o,uint64_t v){
    volatile uint32_t *p=(volatile uint32_t *)(uintptr_t)(XHCI_VIRT+o);
    p[0]=(uint32_t)v; p[1]=(uint32_t)(v>>32);
}
static int map_mmio(uint64_t phys){
    uint64_t a=phys&~(uint64_t)(PAGE_SIZE-1u);
    uint32_t off=(uint32_t)(phys&(PAGE_SIZE-1u));
    uint32_t bytes=(XHCI_MAP_SIZE+off+PAGE_SIZE-1u)&~(PAGE_SIZE-1u);
    for(uint32_t o=0;o<bytes;o+=PAGE_SIZE)
        if(paging_map_kernel_page(XHCI_VIRT+o,a+o,0x13u)!=0) return -1;
    return 0;
}

/*
 * Walk the extended capability list once and:
 *   1. Parse USB Protocol capabilities (which port is USB2/USB3)
 *   2. Enable Intel VSEC USB3 ports (SuperSpeed + SuperSpeedPlus)
 *   3. Dump every extended cap so we can see what the controller offers
 */
static uint32_t xecp_start(void){
    return ((r32(CAP_HCCPARAMS1) >> 16) & 0xFFFFu) * 4u;
}

static void dump_xecp(void){
    uint32_t x = xecp_start();
    if(!x){ usb_log("[ USB ] no extended capabilities\n"); return; }
    usb_log("[ USB ] extended capabilities:\n");
    for(uint32_t i=0;i<64u;++i){
        if(!x || x >= XHCI_MAP_SIZE) break;
        uint32_t cap = r32(x);
        uint32_t id  = cap & 0xFFu;
        uint32_t next = ((cap >> 8) & 0xFFu) * 4u;
        usb_log("[ USB ]   off=0x"); usb_log_hex(x);
        usb_log(" id=0x"); usb_log_hex(id);
        usb_log(" dw0=0x"); usb_log_hex(cap);
        usb_log(" next="); usb_log_dec(next);
        if(id == EXT_USB_PROTOCOL_ID){
            uint32_t dw2 = r32(x+8u);
            usb_log(" [USB-PROTO major="); usb_log_dec((cap>>24)&0xFFu);
            usb_log(" off="); usb_log_dec(dw2 & 0xFFu);
            usb_log(" cnt="); usb_log_dec((dw2>>8)&0xFFu);
            usb_log("]");
        } else if(id == EXT_USB_LEGSUP_ID){
            usb_log(" [USB-LEGSUP]");
        } else if(id == EXT_VENDOR_ID){
            uint32_t vsec_id = (cap >> 16) & 0xFFFFu;
            usb_log(" [VENDOR vsec_id=0x"); usb_log_hex(vsec_id);
            if(vsec_id == INTEL_VSEC_ID){
                uint32_t ports = r32(x + INTEL_VSEC_USB3_PORTS);
                usb_log(" INTEL USB3_PORTS=0x"); usb_log_hex(ports);
            }
            usb_log("]");
        }
        usb_log_nl();
        if(!next) break;
        x += next;
    }
}

static void parse_usb_protocols(void){
    protocol_count=0;
    uint32_t x=xecp_start();
    if(!x) return;
    for(uint32_t i=0;i<64u;++i){
        if(!x||x>=XHCI_MAP_SIZE) break;
        uint32_t cap=r32(x);
        uint32_t id=cap&0xFFu;
        uint32_t next=((cap>>8)&0xFFu)*4u;
        if(id==EXT_USB_PROTOCOL_ID){
            uint32_t dw2=r32(x+8u);
            uint8_t major=(uint8_t)((cap>>24)&0xFFu);
            uint8_t minor=(uint8_t)((cap>>16)&0xFFu);
            uint8_t port_offset=(uint8_t)(dw2&0xFFu);
            uint8_t port_count=(uint8_t)((dw2>>8)&0xFFu);
            uint8_t slot_type=(uint8_t)((dw2>>16)&0xFu);
            uint8_t proto=(major>=3u)?PROTO_USB3:PROTO_USB2;
            if(protocol_count<MAX_PROTOCOLS){
                protocols[protocol_count].major=major;
                protocols[protocol_count].minor=minor;
                protocols[protocol_count].protocol=proto;
                protocols[protocol_count].port_count=port_count;
                protocols[protocol_count].port_offset=port_offset;
                protocols[protocol_count].slot_type=slot_type;
                ++protocol_count;
            }
        }
        if(!next) break;
        x+=next;
    }
}

/*
 * Linux xhci-pci.c usb_enable_intel_xhci_ports():
 *   find Intel VSEC, set bits 0 and 1 of USB3_PORTS register to enable
 *   SuperSpeed and SuperSpeedPlus ports.
 */
static void enable_intel_usb3_ports(void){
    /*
     * Do not modify arbitrary xHCI extended-capability registers here.
     * USB3 port power/link state is controlled through PORTSC and the
     * controller's Supported Protocol capabilities. Older LionOS code
     * treated extended-capability ID 0x0C as an Intel VSEC and wrote to
     * offset +0x0A; that ID is an xHCI-defined capability on real
     * controllers, so that write could corrupt controller state.
     *
     * Intel xHCI controllers do not require this speculative write.
     * Port reset/power sequencing below handles the root ports.
     */
    (void)xhci_vendor_id;
}

static int legacy_handoff(void){
    uint32_t x=xecp_start();
    for(uint32_t i=0;i<64u;++i){
        if(!x||x>=XHCI_MAP_SIZE) break;
        uint32_t cap=r32(x);
        uint32_t id=cap&0xFFu;
        uint32_t next=((cap>>8)&0xFFu)*4u;
        if(id==EXT_USB_LEGSUP_ID){
            w32(x,cap|USBLEGSUP_OS_OWNED);
            if(cap&USBLEGSUP_BIOS_OWNED){
                for(uint32_t k=0;k<10000000u;++k){
                    if(!(r32(x)&USBLEGSUP_BIOS_OWNED)) break;
                    __asm__ volatile("pause");
                }
                if(r32(x)&USBLEGSUP_BIOS_OWNED) return -1;
            }
            w32(x+4u,0u); return 0;
        }
        if(!next) break;
        x+=next;
    }
    return 0;
}

static uint8_t port_protocol(uint32_t p){
    for(uint32_t i=0;i<protocol_count;++i){
        uint32_t start=(uint32_t)protocols[i].port_offset+1u;
        uint32_t end=start+(uint32_t)protocols[i].port_count;
        if(p>=start&&p<end) return protocols[i].protocol;
    }
    return 0;
}

static int hc_reset(void){
    uint32_t cmd=r32(op_base+OP_USBCMD)&~CMD_RUN;
    w32(op_base+OP_USBCMD,cmd);
    for(uint32_t i=0;i<2000000u;++i){ if(r32(op_base+OP_USBSTS)&STS_HCH) break; __asm__ volatile("pause"); }
    if(!(r32(op_base+OP_USBSTS)&STS_HCH)) return -1;
    w32(op_base+OP_USBCMD,cmd|CMD_HCRST);
    xhci_delay_ms(1u);
    for(uint32_t i=0;i<10000000u;++i){ if(!(r32(op_base+OP_USBCMD)&CMD_HCRST)) break; __asm__ volatile("pause"); }
    if(r32(op_base+OP_USBCMD)&CMD_HCRST) return -1;
    for(uint32_t i=0;i<10000000u;++i){ if(!(r32(op_base+OP_USBSTS)&STS_CNR)) break; __asm__ volatile("pause"); }
    return (r32(op_base+OP_USBSTS)&STS_CNR)?-1:0;
}

static int dma_page(void **out){
    void *p=page_alloc();
    if(!p) return -1;
    zero_mem(p,PAGE_SIZE);
    if(out) *out = p;
    return 0;
}
static uint32_t read_scratchpads(void){
    uint32_t h=r32(CAP_HCSPARAMS2);
    return ((h>>21)&0x1Fu)|(((h>>27)&0x1Fu)<<5);
}
static int alloc_memory(void){
    if(dma_page((void**)&dcbaa)||dma_page((void**)&cmd_ring)||
       dma_page((void**)&event_ring)||dma_page((void**)&erst)||
       dma_page((void**)&ep0_ring)||dma_page(&out_ctx)||dma_page(&in_ctx)||
       dma_page((void**)&control_buf)||dma_page((void**)&config_buf)||
       dma_page((void**)&report_buf)) return -1;

    for(uint32_t i=0u;i<INTR_SEGMENTS;++i)
        if(dma_page((void**)&intr_segments[i])) return -1;

    intr_ring=intr_segments[0];

    zero_mem(dcbaa,PAGE_SIZE);
    zero_mem(cmd_ring,PAGE_SIZE);
    zero_mem(event_ring,PAGE_SIZE);
    zero_mem(erst,PAGE_SIZE);
    zero_mem(ep0_ring,PAGE_SIZE);
    zero_mem(out_ctx,PAGE_SIZE);
    zero_mem(in_ctx,PAGE_SIZE);
    zero_mem(control_buf,PAGE_SIZE);
    zero_mem(config_buf,PAGE_SIZE);
    zero_mem(report_buf,PAGE_SIZE);
    for(uint32_t i=0u;i<INTR_SEGMENTS;++i) zero_mem(intr_segments[i],PAGE_SIZE);

    scratch_count=read_scratchpads();
    if(scratch_count>MAX_SCRATCH) return -1;
    if(scratch_count){
        scratch_array=(uint64_t*)page_alloc();
        if(!scratch_array) return -1;
        zero_mem(scratch_array,PAGE_SIZE);
        for(uint32_t i=0u;i<scratch_count;++i){
            scratch_pages[i]=page_alloc();
            if(!scratch_pages[i]) return -1;
            zero_mem(scratch_pages[i],PAGE_SIZE);
            scratch_array[i]=(uint64_t)(uintptr_t)scratch_pages[i];
        }
        dcbaa[0]=(uint64_t)(uintptr_t)scratch_array;
    }
    return 0;
}

static void link_trb(trb_t *ring,trb_t *next,uint32_t toggle){
    trb_t *t=&ring[RING_TRBS-1u];
    uint64_t a=(uint64_t)(uintptr_t)next;
    t->lo=(uint32_t)a; t->hi=(uint32_t)(a>>32); t->status=0;
    /*
     * Linux/xHCI ring rules: every segment Link TRB starts with cycle=0;
     * only the final segment has Toggle Cycle set.  The producer toggles
     * each Link TRB's cycle bit when it crosses that link.  This keeps
     * multi-segment HID rings valid across repeated wraps.
     */
    t->control=(TRB_LINK<<10)|(toggle?TRB_TC:0u);
}
static void advance_link(trb_t *link,uint32_t *ring_cycle){
    uint32_t toggle=link->control&TRB_TC;
    dma_wmb();
    link->control^=TRB_CYCLE;
    if(toggle)*ring_cycle^=1u;
}
static int setup_rings(void){
    cmd_index=0; cmd_cycle=1;
    event_index=0; event_cycle=1;
    ep0_index=0; ep0_cycle=1;
    intr_index=0; intr_cycle=1; intr_segment=0;

    link_trb(cmd_ring,cmd_ring,1u);
    link_trb(ep0_ring,ep0_ring,1u);
    for(uint32_t i=0u;i<INTR_SEGMENTS;++i)
        link_trb(intr_segments[i],intr_segments[(i+1u)%INTR_SEGMENTS],
                 i==(INTR_SEGMENTS-1u));

    erst[0].base=(uint64_t)(uintptr_t)event_ring;
    erst[0].size=EVENT_TRBS;
    erst[0].reserved=0u;

    w64(op_base+OP_CRCR,(uint64_t)(uintptr_t)cmd_ring|1u);
    uint32_t ir=rt_base+RT_IR0;
    w32(ir+IR_IMAN,0u); w32(ir+IR_IMOD,0u); w32(ir+IR_ERSTSZ,1u);
    w64(ir+IR_ERSTBA,(uint64_t)(uintptr_t)erst);
    w64(ir+IR_ERDP,(uint64_t)(uintptr_t)event_ring);
    return 0;
}
static int start_controller(void){
    uint32_t slots=max_slots; if(slots>255u) slots=255u; if(!slots) slots=1u;
    w32(op_base+OP_CONFIG,slots);
    w64(op_base+OP_DCBAAP,(uint64_t)(uintptr_t)dcbaa);
    w32(op_base+OP_USBCMD,r32(op_base+OP_USBCMD)|CMD_RUN);
    for(uint32_t i=0;i<2000000u;++i){ if(!(r32(op_base+OP_USBSTS)&STS_HCH)) return 0; __asm__ volatile("pause"); }
    return -1;
}
static void cmd_submit(uint32_t type,uint64_t p,uint32_t ctl){
    trb_t *t=&cmd_ring[cmd_index];
    t->lo=(uint32_t)p; t->hi=(uint32_t)(p>>32); t->status=0;
    t->control=(type<<10)|ctl|(cmd_cycle?TRB_CYCLE:0u);
    ++cmd_index;
    if(cmd_index>=RING_TRBS-1u){
        advance_link(&cmd_ring[RING_TRBS-1u],&cmd_cycle);
        cmd_index=0u;
    }
    dma_wmb();
    *(volatile uint32_t *)(uintptr_t)(XHCI_VIRT+db_base)=0u;
}
static int next_event(trb_t *out){
    trb_t *t=&event_ring[event_index];
    if((t->control&TRB_CYCLE)!=(event_cycle?TRB_CYCLE:0u)) return -1;
    if(out)*out=*t;
    t->control=0;
    ++event_index;
    if(event_index>=EVENT_TRBS){
        event_index=0u;
        event_cycle^=1u;
    }
    w64(rt_base+RT_IR0+IR_ERDP,
        (uint64_t)(uintptr_t)&event_ring[event_index]|8u);
    return 0;
}
static int cmd_abort(void){
    w32(op_base+OP_USBCMD,r32(op_base+OP_USBCMD)|CMD_INTE);
    for(uint32_t i=0;i<2000000u;++i){ if(!(r32(op_base+OP_USBCMD)&CMD_INTE)) break; __asm__ volatile("pause"); }
    for(uint32_t n=0;n<5000000u;++n){
        trb_t e; if(next_event(&e)!=0){ __asm__ volatile("pause"); continue; }
        if(((e.control>>10)&0x3Fu)!=TRB_CMD_EVT) continue;
        if(((e.status>>24)&0xFFu)==CC_CMD_STOPPED) return 0;
    }
    return -1;
}
static int cmd_recover(void){
    if(cmd_abort()) return -1;
    cmd_cycle^=1u; cmd_index=0;
    link_trb(cmd_ring,cmd_ring,1u);
    w64(op_base+OP_CRCR,(uint64_t)(uintptr_t)cmd_ring|(cmd_cycle?1u:0u));
    return 0;
}
static int cmd_wait(uint32_t want_slot){
    for(uint32_t n=0;n<8000000u;++n){
        trb_t e;
        if(next_event(&e)!=0){ __asm__ volatile("pause"); continue; }
        uint32_t type=(e.control>>10)&0x3Fu;
        if(type==TRB_PORT_EVT) continue;
        if(type==TRB_BW_EVT) continue;
        if(type!=TRB_CMD_EVT) continue;
        if(want_slot&&((e.control>>24)&0xFFu)!=want_slot) continue;
        uint32_t cc=(e.status>>24)&0xFFu;
        if(cc==CC_SUCCESS) return 0;
        if(cc==CC_RING_FULL){ usb_log("[ USB ] ring full\n"); continue; }
        cmd_recover(); return -1;
    }
    cmd_recover(); return -1;
}

#define ICC_SIZE 32u
static uint8_t *in_slot(void){ return (uint8_t*)in_ctx+ICC_SIZE; }
static uint8_t *in_ep(uint32_t dci){ return (uint8_t*)in_ctx+ICC_SIZE+ctx_size*dci; }
static uint8_t *dev_ep(uint32_t dci){ return (uint8_t*)out_ctx+ctx_size*dci; }
static void ctx_set64(void *c,uint32_t dw,uint64_t v){
    uint32_t *p=(uint32_t*)c; p[dw]=(uint32_t)v; p[dw+1]=(uint32_t)(v>>32);
}
static uint32_t ep0_default_mps(void){
    /*
     * Match the xHCI/Linux enumeration model: before the first
     * device descriptor, use the standard 64-byte EP0 context for
     * full-speed devices.  The returned descriptor then supplies
     * the device's actual bMaxPacketSize0.
     */
    if(device_speed==SPEED_HIGH) return 64u;
    if(device_speed>=SPEED_SUPER) return 512u;
    if(device_speed==SPEED_FULL) return 64u;
    return 8u;
}
static void fill_slot_context(void *slot,uint32_t entries){
    uint32_t *s=(uint32_t*)slot; zero_mem(s,ctx_size);
    s[0]=((device_speed&0xFu)<<20)|((entries&0x1Fu)<<27);
    s[1]=(port_number&0xFFu)<<16;
}
static void fill_ep_context(void *ep,uint32_t ep_type,uint32_t mps,
                            uint32_t interval,uint64_t dequeue,uint32_t avg_len){
    uint32_t *e=(uint32_t*)ep; zero_mem(e,ctx_size);
    e[0]=(interval&0xFFu)<<16;
    /*
     * CErr (Endpoint Context DW1 bits 2:1) must be initialized for
     * non-isochronous endpoints.  Use the normal xHCI value of 3.
     * Physical controllers can reject transfers when these bits are
     * left at zero, even when QEMU accepts the context.
     */
    e[1]=(3u<<1)|((ep_type&0x7u)<<3)|((mps&0xFFFFu)<<16);
    ctx_set64(ep,2u,dequeue);
    e[4]=avg_len&0xFFFFu;
}

static int cmd_enable_slot(void){
    cmd_submit(TRB_ENABLE_SLOT,0,0);
    for(uint32_t n=0;n<8000000u;++n){
        trb_t e;
        if(next_event(&e)!=0){ __asm__ volatile("pause"); continue; }
        uint32_t type=(e.control>>10)&0x3Fu;
        if(type==TRB_PORT_EVT) continue;
        if(type!=TRB_CMD_EVT) continue;
        uint32_t cc=(e.status>>24)&0xFFu;
        if(cc!=CC_SUCCESS){ cmd_recover(); return -1; }
        slot_id=(e.control>>24)&0xFFu;
        return slot_id?0:-1;
    }
    cmd_recover(); return -1;
}
static int cmd_address_device(uint32_t bsr){
    zero_mem(in_ctx,PAGE_SIZE);
    dcbaa[slot_id]=(uint64_t)(uintptr_t)out_ctx;
    ((uint32_t*)in_ctx)[1]=3u;
    fill_slot_context(in_slot(),1u);
    uint64_t deq=(uint64_t)(uintptr_t)&ep0_ring[ep0_index];
    deq|=(ep0_cycle?1ull:0ull);
    fill_ep_context(in_ep(1u),4u,ep0_mps,0u,deq,8u);
    uint32_t ctl=slot_id<<24;
    if(bsr) ctl|=TRB_BSR;
    cmd_submit(TRB_ADDRESS_DEVICE,(uint64_t)(uintptr_t)in_ctx,ctl);
    return cmd_wait(slot_id);
}
static int cmd_evaluate_context(void){
    cmd_submit(TRB_EVAL_CONTEXT,(uint64_t)(uintptr_t)in_ctx,slot_id<<24);
    return cmd_wait(slot_id);
}
static int update_ep0_mps(uint32_t mps){
    if(mps==ep0_mps) return 0;
    uint8_t *dst=in_ep(1u); uint8_t *src=dev_ep(1u);
    zero_mem(in_ctx,PAGE_SIZE);
    ((uint32_t*)in_ctx)[1]=1u|(1u<<1);
    fill_slot_context(in_slot(),1u);
    for(uint32_t i=0;i<ctx_size;++i) dst[i]=src[i];
    uint32_t *dw=(uint32_t*)dst;
    dw[1]=(dw[1]&0x0000FFFFu)|((mps&0xFFFFu)<<16);
    if(cmd_evaluate_context()!=0){ usb_log("[ USB ] Eval Ctx fail\n"); return -1; }
    ep0_mps=mps; return 0;
}
static int submit_report(void);
static int cmd_reset_endpoint(uint32_t ep){
    uint32_t ctl=((ep&0x1Fu)<<16)|(slot_id<<24);
    cmd_submit(TRB_RESET_EP,0,ctl);
    return cmd_wait(slot_id);
}
static int cmd_set_endpoint_deq(uint32_t ep,uint64_t deq){
    uint32_t ctl=((ep&0x1Fu)<<16)|(slot_id<<24);
    cmd_submit(TRB_SET_DEQ,deq,ctl);
    return cmd_wait(slot_id);
}
static uint32_t hid_endpoint_state(void){
    if(!ready||!slot_id||!endpoint_id||!dev_ep(endpoint_id)) return 0u;
    return ((uint32_t*)dev_ep(endpoint_id))[0] & 0x7u;
}
static int hid_endpoint_needs_recovery(void){
    uint32_t state=hid_endpoint_state();
    return state==2u || state==3u || state==4u;
}

static int restart_hid_transfer_ring(void){
    if(!ready||!slot_id||!endpoint_id) return -1;

    /* Reset the endpoint before replacing the DMA queue. */
    if(cmd_reset_endpoint(endpoint_id)!=0) return -1;

    for(uint32_t i=0u;i<INTR_SEGMENTS;++i){
        zero_mem(intr_segments[i],PAGE_SIZE);
        link_trb(intr_segments[i],intr_segments[(i+1u)%INTR_SEGMENTS],
                 i==(INTR_SEGMENTS-1u));
    }

    intr_segment=0u;
    intr_index=0u;
    intr_cycle=1u;
    report_pending=0u;

    if(cmd_set_endpoint_deq(endpoint_id,
            (uint64_t)(uintptr_t)intr_segments[0]|1ull)!=0) return -1;

    dma_wmb();
    return submit_report();
}
static int cmd_reset_ep0(void){
    uint32_t ctl=(1u<<16)|(slot_id<<24);
    cmd_submit(TRB_RESET_EP,0,ctl);
    if(cmd_wait(slot_id)) return -1;
    uint64_t deq=(uint64_t)(uintptr_t)&ep0_ring[ep0_index];
    deq|=(ep0_cycle?1ull:0ull);
    cmd_submit(TRB_SET_DEQ,deq,ctl);
    return cmd_wait(slot_id);
}

static int ep0_xfer(uint8_t bm,uint8_t req,uint16_t val,uint16_t idx,
                    void *data,uint16_t len,int in){
    uint64_t setup=(uint64_t)bm|((uint64_t)req<<8)|((uint64_t)val<<16)
                  |((uint64_t)idx<<32)|((uint64_t)len<<48);
    uint32_t trt=len?(in?(3u<<16):(2u<<16)):0u;
    trb_t *t=&ep0_ring[ep0_index];

    /*
     * Follow the xHCI control-transfer model used by Linux:
     * Setup/Data/Status are one control TD and are sequenced by the
     * control-transfer engine.  Do not add TRB_CHAIN to these stages.
     */
    t->lo=(uint32_t)setup; t->hi=(uint32_t)(setup>>32); t->status=8u;
    t->control=(TRB_SETUP<<10)|TRB_IDT|trt|(ep0_cycle?TRB_CYCLE:0u);
    ++ep0_index;
    if(ep0_index>=RING_TRBS-1u){
        advance_link(&ep0_ring[RING_TRBS-1u],&ep0_cycle);
        ep0_index=0u;
    }

    if(len){
        t=&ep0_ring[ep0_index];
        t->lo=(uint32_t)(uintptr_t)data;
        t->hi=(uint32_t)((uint64_t)(uintptr_t)data>>32);
        t->status=len&0x1FFFFu;
        t->control=(TRB_DATA<<10)|(in?TRB_DIR_IN:0u)|
                   (in?TRB_ISP:0u)|(ep0_cycle?TRB_CYCLE:0u);
        ++ep0_index;
        if(ep0_index>=RING_TRBS-1u){ link_trb(ep0_ring,ep0_ring,1u); ep0_index=0; ep0_cycle^=1u; }
    }

    t=&ep0_ring[ep0_index];
    t->lo=0; t->hi=0; t->status=0;
    t->control=(TRB_STATUS<<10)|TRB_IOC|(in?0u:TRB_DIR_IN)|
               (ep0_cycle?TRB_CYCLE:0u);
    ++ep0_index;
    if(ep0_index>=RING_TRBS-1u){ link_trb(ep0_ring,ep0_ring,1u); ep0_index=0; ep0_cycle^=1u; }

    dma_wmb();
    *(volatile uint32_t *)(uintptr_t)(XHCI_VIRT+db_base+slot_id*4u)=1u;

    for(uint32_t n=0;n<8000000u;++n){
        trb_t e;
        if(next_event(&e)!=0){ __asm__ volatile("pause"); continue; }
        if(((e.control>>10)&0x3Fu)!=TRB_TRANSFER_EVT) continue;
        if(((e.control>>24)&0xFFu)!=slot_id) continue;
        if(((e.control>>16)&0x1Fu)!=1u) continue;
        uint32_t cc=(e.status>>24)&0xFFu;
        diag_last_cc=cc;
        if(cc==CC_SUCCESS||cc==CC_SHORT_PKT) return 0;
        if(cmd_reset_ep0()==0u) return -2;
        return -1;
    }
    return -1;
}
static int ctrl(uint8_t bm,uint8_t req,uint16_t val,uint16_t idx,
                void *data,uint16_t len,int in,uint32_t retries){
    for(uint32_t i=0;i<=retries;++i){
        int rc=ep0_xfer(bm,req,val,idx,data,len,in);
        if(rc==0) return 0;
        if(i<retries){
            xhci_delay_ms(50u);
            if(rc==-2){ ep0_index=0; ep0_cycle=1; cmd_reset_ep0(); xhci_delay_ms(20u); }
        } else return rc;
    }
    return -1;
}

static int read_device_descriptor(int already_addressed){
    zero_mem(control_buf,PAGE_SIZE);
    int rc=ctrl(0x80u,6u,0x0100u,0,control_buf,8u,1,2);
    if(rc!=0){
        xhci_delay_ms(250u);
        ep0_index=0; ep0_cycle=1; (void)cmd_reset_ep0(); xhci_delay_ms(50u);
        zero_mem(control_buf,PAGE_SIZE);
        if(ctrl(0x80u,6u,0x0100u,0,control_buf,8u,1,3)) return -1;
    }
    if(control_buf[0]<8u || control_buf[1]!=1u) return -1;

    uint32_t mps=control_buf[7];
    if(mps==9u) mps=512u;
    else if(mps!=8u && mps!=16u && mps!=32u && mps!=64u)
        mps=(device_speed>=SPEED_SUPER)?512u:8u;
    ep0_mps=mps;
    usb_log("[ USB ] ep0_mps(first)="); usb_log_dec(ep0_mps); usb_log_nl();

    if(!already_addressed){
        if(cmd_address_device(0)) return -1;
        xhci_delay_ms(200u);
        ep0_index=0; ep0_cycle=1; cmd_reset_ep0(); xhci_delay_ms(50u);
    }

    zero_mem(control_buf,PAGE_SIZE);
    rc=ctrl(0x80u,6u,0x0100u,0,control_buf,18u,1,3);
    if(rc!=0){
        usb_log("[ USB ] retry 18-byte with MPS=8\n");
        if(update_ep0_mps(8u)==0){
            xhci_delay_ms(50u);
            ep0_index=0; ep0_cycle=1; (void)cmd_reset_ep0(); xhci_delay_ms(50u);
            zero_mem(control_buf,PAGE_SIZE);
            rc=ctrl(0x80u,6u,0x0100u,0,control_buf,18u,1,3);
        }
        if(rc) return -1;
    }
    if(control_buf[0]<18u || control_buf[1]!=1u) return -1;

    console_write("[ USB ] dev desc:");
    for(int i=0;i<18;++i){ console_putc(' '); console_write_hex(control_buf[i]); }
    console_putc('\n');

    uint32_t mps2=control_buf[7];
    if(mps2==9u) mps2=512u;
    else if(mps2!=8u && mps2!=16u && mps2!=32u && mps2!=64u) mps2=ep0_mps;
    usb_log("[ USB ] ep0_mps(second)="); usb_log_dec(mps2); usb_log_nl();
    if(mps2!=ep0_mps){
        if(update_ep0_mps(mps2)==0){
            usb_log("[ USB ] EP0 MPS updated\n");
            xhci_delay_ms(50u);
        }
    }

    diag_vid=(uint16_t)control_buf[8]|((uint16_t)control_buf[9]<<8);
    diag_pid=(uint16_t)control_buf[10]|((uint16_t)control_buf[11]<<8);
    diag_device_class=control_buf[4];
    diag_device_subclass=control_buf[5];
    diag_device_protocol=control_buf[6];
    return 0;
}

static int read_config_descriptor(void){
    for(uint32_t attempt=0;attempt<4;++attempt){
        zero_mem(config_buf,PAGE_SIZE);
        int rc=ctrl(0x80u,6u,0x0200u,0,config_buf,9u,1,1);
        if(rc==0 && config_buf[1]==2u && config_buf[0]>=9u){
            uint16_t total=(uint16_t)config_buf[2]|((uint16_t)config_buf[3]<<8);
            if(total>=9u && total<=4096u){
                xhci_delay_ms(20u);
                rc=ctrl(0x80u,6u,0x0200u,0,config_buf,total,1,2);
                if(rc==0 && config_buf[1]==2u && config_buf[0]>=9u) return 0;
            }
        }
        usb_log("[ USB ] cfg retry "); usb_log_dec(attempt);
        usb_log(" rc="); usb_log_dec((uint32_t)rc);
        usb_log(" b0="); usb_log_hex(config_buf[0]);
        usb_log(" b1="); usb_log_hex(config_buf[1]);
        usb_log_nl();
        xhci_delay_ms(100u);
        ep0_index=0; ep0_cycle=1; cmd_reset_ep0(); xhci_delay_ms(50u);
    }
    return -1;
}

static void dump_config(void){
    usb_log("\n[ USB DEBUG ] DEVICE\n");
    usb_log(" VID=0x"); usb_log_hex(diag_vid);
    usb_log(" PID=0x"); usb_log_hex(diag_pid);
    usb_log(" CLASS=0x"); usb_log_hex(diag_device_class);
    usb_log(" SUB=0x"); usb_log_hex(diag_device_subclass);
    usb_log(" PROTO=0x"); usb_log_hex(diag_device_protocol);
    usb_log("\n[ USB DEBUG ] CONFIG\n");
    diag_interface_count=diag_endpoint_count=diag_hid_count=0;
    diag_hid_iface=0xFFu; diag_hid_subclass=diag_hid_protocol=0;
    diag_hid_endpoint=diag_hid_ep_type=0; diag_hid_packet=0; diag_hid_interval=0;
    uint16_t total=(uint16_t)config_buf[2]|((uint16_t)config_buf[3]<<8);
    if(total>4096u) total=4096u;
    uint32_t i=0; uint8_t cur=0xFFu; uint8_t cur_hid=0;
    while(i+2u<=total){
        uint8_t len=config_buf[i], type=config_buf[i+1u];
        if(len<2u||i+len>total) break;
        if(type==4u&&len>=9u){
            cur=config_buf[i+2u];
            uint8_t alt=config_buf[i+3u];
            uint8_t cls=config_buf[i+5u];
            uint8_t sub=config_buf[i+6u];
            uint8_t proto=config_buf[i+7u];
            cur_hid=(alt==0u&&cls==3u)?1u:0u;
            ++diag_interface_count;
            usb_log(" IF="); usb_log_dec(cur);
            usb_log(" ALT="); usb_log_dec(alt);
            usb_log(" CLASS=0x"); usb_log_hex(cls);
            usb_log(" SUB=0x"); usb_log_hex(sub);
            usb_log(" PROTO=0x"); usb_log_hex(proto); usb_log_nl();
            if(cur_hid){
                ++diag_hid_count;
                if(diag_hid_iface==0xFFu){ diag_hid_iface=cur; diag_hid_subclass=sub; diag_hid_protocol=proto; }
            }
        } else if(type==5u&&len>=7u){
            uint8_t addr=config_buf[i+2u];
            uint8_t attr=config_buf[i+3u];
            uint16_t mps=(uint16_t)config_buf[i+4u]|((uint16_t)config_buf[i+5u]<<8);
            ++diag_endpoint_count;
            usb_log("  EP=0x"); usb_log_hex(addr);
            usb_log(" TYPE="); usb_log_dec(attr&3u);
            usb_log(" MPS="); usb_log_dec(mps&0x7FFu);
            usb_log(" INT="); usb_log_dec(config_buf[i+6u]); usb_log_nl();
            if(cur_hid && !diag_hid_endpoint && (addr&0x80u) && (attr&3u)==3u){
                diag_hid_endpoint=addr; diag_hid_ep_type=attr&3u;
                diag_hid_packet=mps&0x7FFu; diag_hid_interval=config_buf[i+6u];
            }
        }
        i+=len;
    }
    usb_log(" SUMMARY IF="); usb_log_dec(diag_interface_count);
    usb_log(" EP="); usb_log_dec(diag_endpoint_count);
    usb_log(" HID="); usb_log_dec(diag_hid_count); usb_log_nl();
}

static int find_hid(hid_candidate_t *c){
    if(read_config_descriptor()) return -1;
    uint16_t total=(uint16_t)config_buf[2]|((uint16_t)config_buf[3]<<8);
    if(total<9u) return -1;
    if(total>4096u) total=4096u;
    dump_config();
    zero_mem(c,sizeof(*c));
    c->config_value=config_buf[5];
    for(uint32_t pass=0;pass<2u;++pass){
        uint32_t i=0; int sel=0;
        while(i+2u<=total){
            uint8_t len=config_buf[i], type=config_buf[i+1u];
            if(len<2u||i+len>total) break;
            if(type==4u&&len>=9u){
                uint8_t alt=config_buf[i+3u];
                uint8_t cls=config_buf[i+5u];
                uint8_t sub=config_buf[i+6u];
                uint8_t proto=config_buf[i+7u];
                sel=(pass==0u)?(alt==0u&&cls==3u&&sub==1u&&proto==2u):(alt==0u&&cls==3u);
                if(sel) c->interface_number=config_buf[i+2u];
            } else if(type==5u&&len>=7u&&sel){
                uint8_t addr=config_buf[i+2u];
                uint8_t attr=config_buf[i+3u];
                uint16_t mps=(uint16_t)config_buf[i+4u]|((uint16_t)config_buf[i+5u]<<8);
                if((addr&0x80u)&&(attr&3u)==3u&&(mps&0x7FFu)){
                    c->endpoint_address=addr;
                    c->packet_size=mps&0x7FFu;
                    c->interval=config_buf[i+6u]?config_buf[i+6u]:1u;
                    return 0;
                }
            }
            i+=len;
        }
    }
    return -1;
}

static uint32_t interval_encode(uint32_t v){
    if(!v) v=1;
    if(device_speed>=SPEED_HIGH){
        if(v>16u) v=16u;
        return v-1u;
    }
    uint32_t uf=v*8u;
    if(uf<8u) uf=8u;
    if(uf>1024u) uf=1024u;
    uint32_t e=3u;
    while(e<10u && (1u<<(e+1u))<=uf) ++e;
    return e;
}
static int cmd_configure_hid(hid_candidate_t *c){
    uint32_t n=c->endpoint_address&0x0Fu;
    if(!n||!(c->endpoint_address&0x80u)) return -1;
    endpoint_id=n*2u+1u;
    if(endpoint_id>=32u) return -1;
    endpoint_packet=c->packet_size;
    if(endpoint_packet>1024u) endpoint_packet=1024u;
    endpoint_interval=interval_encode(c->interval);
    zero_mem(in_ctx,PAGE_SIZE);
    ((uint32_t*)in_ctx)[1]=1u|(1u<<endpoint_id);
    fill_slot_context(in_slot(),endpoint_id);
    uint64_t deq=(uint64_t)(uintptr_t)intr_ring|(intr_cycle?1ull:0ull);
    fill_ep_context(in_ep(endpoint_id),7u,endpoint_packet,endpoint_interval,deq,endpoint_packet);
    cmd_submit(TRB_CONFIGURE_EP,(uint64_t)(uintptr_t)in_ctx,slot_id<<24);
    return cmd_wait(slot_id);
}
static int submit_report(void){
    if(!endpoint_id) return -1;
    report_length=endpoint_packet;
    if(report_length<3u) report_length=3u;
    if(report_length>PAGE_SIZE) report_length=PAGE_SIZE;
    if(report_pending) return 0;

    trb_t *t=&intr_segments[intr_segment][intr_index];
    t->lo=(uint32_t)(uintptr_t)report_buf;
    t->hi=(uint32_t)((uint64_t)(uintptr_t)report_buf>>32);
    t->status=report_length&0x1FFFFu;
    t->control=(TRB_NORMAL<<10)|TRB_IOC|TRB_ISP|
               (intr_cycle?TRB_CYCLE:0u);

    ++intr_index;
    if(intr_index>=RING_TRBS-1u){
        advance_link(&intr_segments[intr_segment][RING_TRBS-1u],&intr_cycle);
        intr_index=0u;
        ++intr_segment;
        if(intr_segment>=INTR_SEGMENTS)
            intr_segment=0u;
    }

    dma_wmb();
    *(volatile uint32_t *)(uintptr_t)(XHCI_VIRT+db_base+slot_id*4u)=endpoint_id;
    report_pending=1u;
    ++diag_transfer_submitted;
    return 0;
}

static void wake_all_ports(void){
    for(uint32_t p=1;p<=max_ports;++p){
        uint32_t po=op_base+OP_PORT_BASE+(p-1u)*OP_PORT_STRIDE;
        uint32_t ps=r32(po);
        uint32_t chg=ps&PS_CHANGE_BITS;
        if(chg) w32(po,port_state_neutral(ps)|chg);
        ps=r32(po);
        if(!(ps&PS_PP)) w32(po,port_state_neutral(ps)|PS_PP|(ps&PS_CHANGE_BITS));
    }
    xhci_delay_ms(50u);
}
static void dump_ports(void){
    for(uint32_t p=1;p<=max_ports;++p){
        uint32_t po=op_base+OP_PORT_BASE+(p-1u)*OP_PORT_STRIDE;
        uint32_t ps=r32(po);
        uint32_t sp=(ps&PS_SPEED_MASK)>>PS_SPEED_SHIFT;
        uint32_t pls=(ps>>5)&0xFu;
        usb_log("[ USB ] port "); usb_log_dec(p);
        usb_log(" PORTSC=0x"); usb_log_hex(ps);
        usb_log(" spd="); usb_log_dec(sp);
        usb_log(" pls="); usb_log_dec(pls);
        if(ps&PS_CCS) usb_log(" CCS");
        if(ps&PS_PED) usb_log(" PED");
        if(ps&PS_PP)  usb_log(" PP");
        usb_log_nl();
    }
}

static void clear_change_bits(uint32_t p,uint32_t ps);

static int port_reset(uint32_t p){
    uint32_t po=op_base+OP_PORT_BASE+(p-1u)*OP_PORT_STRIDE;
    uint32_t ps=r32(po);
    if(!(ps&PS_CCS)) return -1;

    /*
     * For a connected device, PORTSC Speed determines the reset type.
     * USB2/full-speed devices need ordinary Port Reset (PR).  SuperSpeed
     * devices need Warm Port Reset (WPR).
     *
     * Do not rewrite PLS/LWS on USB2 ports.  Those fields are not part of
     * the USB2 reset operation and changing them can disturb a real
     * controller's USB2 link while the device is being enumerated.
     */
    uint32_t sp=(ps&PS_SPEED_MASK)>>PS_SPEED_SHIFT;
    int is_usb3;
    if(sp>=SPEED_SUPER) is_usb3=1;
    else if(sp!=SPEED_UNDEF) is_usb3=0;
    else is_usb3=(port_protocol(p)==PROTO_USB3);

    usb_log("[ USB ] reset port "); usb_log_dec(p);
    usb_log(" speed="); usb_log_dec(sp);
    usb_log(" mode="); usb_log(is_usb3?"USB3-WPR":"USB2-PR");
    usb_log_nl();

    uint32_t chg=ps&PS_CHANGE_BITS;
    if(chg) w32(po,(ps&~PS_CHANGE_BITS)|chg);
    (void)r32(po);

    ps=r32(po);

    if(is_usb3){
        /* Warm Port Reset: preserve the controller-reported link state. */
        uint32_t v=port_state_neutral(ps);
        if(ppc_enabled) v|=PS_PP;
        v|=PS_WPR;
        w32(po,v);
    } else {
        /* USB2 Port Reset: PR is the only reset control we need. */
        uint32_t v=port_state_neutral(ps);
        if(ppc_enabled) v|=PS_PP;
        v|=PS_PR;
        w32(po,v);
    }
    (void)r32(po);

    /*
     * Give the device time to complete reset, then wait for the
     * controller to report an attached/enabled USB2 device.
     */
    xhci_delay_ms(50u);

    for(uint32_t n=0;n<20000000u;++n){
        uint32_t q=r32(po);

        if(is_usb3){
            if(!(q&PS_WPR) && (q&PS_CCS) &&
               (q&PS_PLS_MASK)==PS_PLS_U0){
                clear_change_bits(p,q);
                return 0;
            }
        } else {
            if((q&PS_CCS) && (q&PS_PED) && !(q&PS_PR)){
                clear_change_bits(p,q);
                return 0;
            }
        }
        __asm__ volatile("pause");
    }

    /*
     * One controlled retry.  Do not force PLS/U0 on USB2.
     */
    ps=r32(po);
    if(!(ps&PS_CCS)) return -1;

    if(is_usb3){
        uint32_t v=port_state_neutral(ps);
        if(ppc_enabled) v|=PS_PP;
        v|=PS_WPR;
        w32(po,v);
    } else {
        uint32_t v=port_state_neutral(ps);
        if(ppc_enabled) v|=PS_PP;
        v|=PS_PR;
        w32(po,v);
    }

    xhci_delay_ms(100u);

    for(uint32_t n=0;n<10000000u;++n){
        uint32_t q=r32(po);
        if(is_usb3){
            if(!(q&PS_WPR) && (q&PS_CCS) &&
               (q&PS_PLS_MASK)==PS_PLS_U0){
                clear_change_bits(p,q);
                return 0;
            }
        } else {
            if((q&PS_CCS) && (q&PS_PED) && !(q&PS_PR)){
                clear_change_bits(p,q);
                return 0;
            }
        }
        __asm__ volatile("pause");
    }

    return -1;
}
static void clear_change_bits(uint32_t p,uint32_t ps){
    uint32_t po=op_base+OP_PORT_BASE+(p-1u)*OP_PORT_STRIDE;
    uint32_t chg=ps&PS_CHANGE_BITS;
    if(chg) w32(po,port_state_neutral(ps)|chg);
}

static int enumerate_port(uint32_t p){
    uint32_t po=op_base+OP_PORT_BASE+(p-1u)*OP_PORT_STRIDE;
    uint32_t ps=r32(po);
    if(!(ps&PS_CCS)) return -1;

    port_number=p;
    diag_last_portsc=ps;
    diag_stage="USB PORT CONNECTED";

    uint8_t proto=port_protocol(p);
    usb_log("[ USB ] port "); usb_log_dec(p);
    usb_log(" proto="); usb_log_dec(proto?proto:99u);
    usb_log(" PORTSC=0x"); usb_log_hex(ps); usb_log_nl();

    diag_stage="PORT RESET";
    if(port_reset(p)){
        diag_last_portsc=r32(po);
        return usb_fail("PORT RESET");
    }

    ps=r32(po);
    diag_last_portsc=ps;
    if(!(ps&PS_CCS)) return usb_fail("PORT DISCONNECTED");
    clear_change_bits(p,ps);
    ps=r32(po);
    diag_last_portsc=ps;
    device_speed=(ps&PS_SPEED_MASK)>>PS_SPEED_SHIFT;
    if(!device_speed) return usb_fail("NO USB SPEED");
    usb_log("[ USB ] speed="); usb_log_dec(device_speed); usb_log_nl();

    ep0_mps=ep0_default_mps();
    usb_log("[ USB ] EP0 default MPS="); usb_log_dec(ep0_mps); usb_log_nl();

    diag_stage="ENABLE SLOT";
    if(cmd_enable_slot()) return usb_fail("ENABLE SLOT");
    xhci_delay_ms(20u);

    int is_usb3=(device_speed>=SPEED_SUPER);

    /*
     * USB 2.0/full/high-speed devices use the normal Address Device
     * command.  The default EP0 MPS is 8 bytes, which is valid for the
     * initial address phase.  Once the device descriptor is returned,
     * read_device_descriptor() updates the EP0 MPS before requesting
     * the remaining descriptor bytes.
     *
     * BSR is intentionally NOT used for this USB2 path.  It is a
     * SuperSpeed-specific xHCI mechanism and is not required for a
     * full-speed HID mouse.
     */
    diag_stage=is_usb3?"ADDRESS USB3":"ADDRESS USB2 FULL/HIGH";
    if(cmd_address_device(is_usb3?1u:0u)){
        return usb_fail(diag_stage);
    }
    xhci_delay_ms(is_usb3?150u:100u);

    /*
     * The device already has its USB address.  read_device_descriptor()
     * therefore performs GET_DESCRIPTOR at that address and handles the
     * EP0 MPS discovery/update.
     */
    int already_addressed=1;

    diag_stage="DEVICE DESCRIPTOR";
    if(read_device_descriptor(already_addressed)){
        usb_log("[ USB ] device descriptor failed cc=0x"); usb_log_hex(diag_last_cc);
        usb_log(" portsc=0x"); usb_log_hex(r32(po)); usb_log_nl();
        diag_last_portsc=r32(po);
        return usb_fail("DEVICE DESCRIPTOR");
    }

    diag_stage="HID CONFIGURATION";
    hid_candidate_t c;
    if(find_hid(&c)||!c.config_value)
        return usb_fail("HID CONFIGURATION");

    diag_stage="SET CONFIGURATION";
    int rc=-1;
    for(uint32_t i=0;i<3u;++i){
        rc=ctrl(0u,9u,c.config_value,0,0,0,0,1);
        if(rc==0) break;
        usb_log("[ USB ] SET CONFIG retry "); usb_log_dec(i+1u);
        usb_log(" cc=0x"); usb_log_hex(diag_last_cc); usb_log_nl();
        xhci_delay_ms(50u);
        ep0_index=0; ep0_cycle=1;
        if(cmd_reset_ep0()) break;
    }
    if(rc) return usb_fail("SET CONFIGURATION");

    diag_stage="CONFIGURE HID EP";
    if(cmd_configure_hid(&c)) return usb_fail("CONFIGURE HID EP");

    diag_stage="HID CLASS SETUP";
    if(ctrl(0x21u,0x0Bu,0u,c.interface_number,0,0,0,1)){
        usb_log("[ USB ] SET_PROTOCOL failed cc=0x"); usb_log_hex(diag_last_cc); usb_log_nl();
    }
    if(ctrl(0x21u,0x0Au,0u,c.interface_number,0,0,0,1)){
        usb_log("[ USB ] SET_IDLE failed cc=0x"); usb_log_hex(diag_last_cc); usb_log_nl();
    }

    endpoint_packet=c.packet_size;
    if(endpoint_packet>PAGE_SIZE) endpoint_packet=PAGE_SIZE;

    usb_log("[ USB ] mouse: iface="); usb_log_dec(c.interface_number);
    usb_log(" ep=0x"); usb_log_hex(c.endpoint_address);
    usb_log(" pkt="); usb_log_dec(endpoint_packet);
    usb_log(" int="); usb_log_dec(c.interval); usb_log_nl();

    diag_stage="SUBMIT HID REPORT";
    report_pending=0; report_length=0; report_seen=0;
    if(submit_report()) return usb_fail("SUBMIT HID REPORT");

    diag_stage="HID REPORT WAIT"; ready=1; diag_init_ok=1;
    usb_log("[ OK ] xHCI HID mouse ready on port ");
    usb_log_dec(port_number); usb_log_nl();
    debug_write("LIONOS:USB-MOUSE-READY\n");
    return 0;
}
int xhci_mouse_init(void){
    if(ready) return 0;
    diag_stage="SCANNING PCI";
    diag_controller_found=0; diag_init_ok=0;
    diag_transfer_submitted=diag_event_count=0;
    diag_success_count=diag_error_count=diag_report_count=0;
    diag_last_cc=diag_last_portsc=diag_last_report_len=0;
    diag_vid=diag_pid=0;
    diag_device_class=diag_device_subclass=diag_device_protocol=0;
    diag_hid_iface=0xFFu; diag_hid_subclass=diag_hid_protocol=0;
    diag_hid_endpoint=diag_hid_ep_type=0; diag_hid_packet=0; diag_hid_interval=0;
    diag_interface_count=diag_endpoint_count=diag_hid_count=0;
    zero_mem(diag_last_report,8);

    pci_dev_t d;
    if(pci_find_xhci(&d)) return usb_fail("PCI");
    diag_controller_found=1; diag_stage="xHCI FOUND";
    xhci_vendor_id = d.vendor;

    usb_log("[ USB ] xHCI controller ");
    console_write_hex(d.vendor); console_putc(':');
    console_write_hex(d.device); console_putc('\n');

    uint32_t pcicmd=pci_r32(d.bus,d.slot,d.function,PCI_COMMAND);
    pcicmd|=0x6u;
    pci_w32(d.bus,d.slot,d.function,PCI_COMMAND,pcicmd);

    diag_stage="MAPPING MMIO";
    if(map_mmio(d.bar0)) return usb_fail("MMIO");

    cap_len=r32(CAP_CAPLENGTH)&0xFFu;
    hci_version=(r32(CAP_CAPLENGTH)>>16)&0xFFFFu;
    if(cap_len<0x20u) return usb_fail("CAP");

    op_base=cap_len;
    db_base=r32(CAP_DBOFF)&~3u;
    rt_base=r32(CAP_RTSOFF)&~0x1Fu;

    uint32_t hcs1=r32(CAP_HCSPARAMS1);
    max_slots=hcs1&0xFFu; max_ports=(hcs1>>24)&0xFFu;
    uint32_t hcc1=r32(CAP_HCCPARAMS1);
    ctx_size=(hcc1&4u)?64u:32u;
    ppc_enabled=(hcc1&8u)?1u:0u;

    if(!max_slots||!max_ports||max_ports>MAX_PORTS) return usb_fail("PARAMS");
    if((r32(op_base+OP_PAGESIZE)&1u)==0u) return usb_fail("PAGESIZE");

    usb_log("[ USB ] xHCI ver "); console_write_hex(hci_version);
    usb_log(" / ctx "); console_write_dec(ctx_size);
    usb_log(" / ports "); console_write_dec(max_ports);
    usb_log(" / ppc "); console_write_dec(ppc_enabled); usb_log_nl();

    /* Dump every extended capability BEFORE we do anything that could
     * clobber the controller state. */
    dump_xecp();

    diag_stage="FIRMWARE HANDOFF";
    if(legacy_handoff()) return usb_fail("LEGACY");

    /* Linux: usb_enable_intel_xhci_ports() — enable SuperSpeed ports */
    diag_stage="INTEL VSEC ENABLE";
    enable_intel_usb3_ports();

    diag_stage="RESETTING xHCI";
    if(hc_reset()) return usb_fail("RESET");

    diag_stage="ALLOCATING DMA";
    if(alloc_memory()) return usb_fail("ALLOC");

    diag_stage="PARSING PROTOCOLS";
    parse_usb_protocols();
    for(uint32_t i=0;i<protocol_count;++i){
        usb_log("[ USB ] proto ");
        usb_log_dec(protocols[i].major); usb_log(".");
        usb_log_dec(protocols[i].minor); usb_log(" ports=");
        usb_log_dec(protocols[i].port_count); usb_log(" off=");
        usb_log_dec(protocols[i].port_offset); usb_log_nl();
    }

    diag_stage="SETUP RINGS";
    setup_rings();

    diag_stage="STARTING xHCI";
    if(start_controller()) return usb_fail("RUN");

    diag_stage="NOOP VERIFY";
    cmd_submit(TRB_NOOP_CMD,0,0);
    if(cmd_wait(0)) return usb_fail("NOOP-CMD");

    diag_stage="PORT POWER";
    wake_all_ports();
    xhci_delay_ms(100u);

    diag_stage="PORT DUMP";
    dump_ports();

    for(uint32_t pass=0;pass<2u;++pass){
        uint32_t saw_ccs=0;
        for(uint32_t p=1;p<=max_ports;++p){
            uint32_t po=op_base+OP_PORT_BASE+(p-1u)*OP_PORT_STRIDE;
            uint32_t ps=r32(po);
            diag_last_portsc=ps;
            if(!(ps&PS_CCS)) continue;
            saw_ccs=1;
            diag_stage="USB PORT CONNECTED";
            usb_log("[ USB ] --- enumerating port "); usb_log_dec(p);
            usb_log(" ---\n");
            if(enumerate_port(p)==0) return 0;
            usb_log("[ USB ] port "); usb_log_dec(p);
            usb_log(" failed, moving on\n");
        }
        if(saw_ccs) return usb_fail(diag_stage);
        if(pass==0u){
            usb_log("[ USB ] no devices, retry after wake\n");
            wake_all_ports();
            xhci_delay_ms(300u);
        } else {
            return usb_fail("NO-PORT");
        }
    }
    return usb_fail("NO-HID-MOUSE");
}

int xhci_mouse_recover(void){
    if(!ready||!slot_id||!endpoint_id) return -1;
    report_pending=0u;
    return restart_hid_transfer_ring();
}
int xhci_mouse_poll(int32_t *dx,int32_t *dy,uint8_t *buttons){
    if(dx) *dx = 0;
    if(dy) *dy = 0;
    if(buttons) *buttons = 0;
    if(!ready) return 0;
    if(hid_endpoint_needs_recovery()) return -1;

    trb_t e;
    while(next_event(&e)==0){
        if(((e.control>>10)&0x3Fu)!=TRB_TRANSFER_EVT) continue;
        if(((e.control>>24)&0xFFu)!=slot_id) continue;
        if(((e.control>>16)&0x1Fu)!=endpoint_id) continue;

        report_pending=0;
        ++diag_event_count;
        uint32_t cc=(e.status>>24)&0xFFu;
        diag_last_cc=cc;

        if(cc==CC_SUCCESS||cc==CC_SHORT_PKT){
            if(report_length>=3u){
                uint32_t residual=e.status&0xFFFFFFu;
                uint32_t actual=report_length>residual?report_length-residual:0u;
                if(actual>PAGE_SIZE) actual=PAGE_SIZE;
                diag_last_report_len=actual>8u?8u:actual;
                for(uint32_t i=0u;i<8u;++i)
                    diag_last_report[i]=(i<diag_last_report_len)?report_buf[i]:0u;

                if(diag_last_report_len>=3u){
                    ++diag_report_count;
                    ++diag_success_count;
                    if(!report_seen){
                        report_seen=1;
                        usb_log("[ OK ] HID mouse reports active\n");
                    }
                    if(buttons) *buttons=diag_last_report[0]&7u;
                    if(dx) *dx=(int32_t)(int8_t)diag_last_report[1];
                    if(dy) *dy=(int32_t)(int8_t)diag_last_report[2];
                }
            }
            if(submit_report()!=0) return -1;
            return 1;
        }

        ++diag_error_count;
        return -1;
    }

    /*
     * A pending interrupt-IN transfer with no completion is normal for an
     * idle HID mouse: the endpoint remains armed until the device has data.
     * Do not reset the endpoint merely because several timer ticks passed.
     */
    if(!report_pending){
        if(submit_report()!=0) return -1;
    }
    return 0;
}
int xhci_mouse_debug_get(xhci_mouse_debug_info_t *out){
    if(!out) return -1;
    out->controller_found=diag_controller_found;
    out->initialized=diag_init_ok;
    out->ready=ready;
    out->port=port_number;
    out->speed=device_speed;
    out->slot=slot_id;
    out->endpoint_id=endpoint_id;
    out->endpoint_address=endpoint_id?(((endpoint_id-1u)/2u)|0x80u):0u;
    out->packet_size=endpoint_packet;
    out->interval=endpoint_interval;
    out->vid=diag_vid; out->pid=diag_pid;
    out->device_class=diag_device_class;
    out->device_subclass=diag_device_subclass;
    out->device_protocol=diag_device_protocol;
    out->hid_iface=diag_hid_iface;
    out->hid_subclass=diag_hid_subclass;
    out->hid_protocol=diag_hid_protocol;
    out->hid_endpoint=diag_hid_endpoint;
    out->hid_ep_type=diag_hid_ep_type;
    out->hid_packet=diag_hid_packet;
    out->hid_interval=diag_hid_interval;
    out->interfaces=diag_interface_count;
    out->endpoints=diag_endpoint_count;
    out->hid_interfaces=diag_hid_count;
    out->submitted=diag_transfer_submitted;
    out->events=diag_event_count;
    out->successes=diag_success_count;
    out->errors=diag_error_count;
    out->reports=diag_report_count;
    out->last_completion=diag_last_cc;
    out->portsc=diag_last_portsc;
    out->stage=diag_stage;
    out->report_len=diag_last_report_len;
    for(uint32_t i=0;i<8;++i) out->report[i]=diag_last_report[i];
    if(diag_controller_found&&op_base){
        out->usb_status=r32(op_base+OP_USBSTS);
        out->usb_command=r32(op_base+OP_USBCMD);
    } else {
        out->usb_status=0;
        out->usb_command=0;
    }
    return 0;
}
