#ifndef LIONOS_XHCI_H
#define LIONOS_XHCI_H
#include <stdint.h>
typedef struct {
    uint32_t controller_found,initialized,ready,port,speed,slot;
    uint32_t endpoint_id,endpoint_address,packet_size,interval;
    uint32_t submitted,events,successes,errors,reports,last_completion;
    uint32_t portsc,usb_status,usb_command;
    const char *stage;
    uint32_t report_len;
    uint8_t report[8];
} xhci_mouse_debug_info_t;
int xhci_mouse_init(void);
int xhci_mouse_poll(int32_t *dx,int32_t *dy,uint8_t *buttons);
int xhci_mouse_debug_get(xhci_mouse_debug_info_t *out);
#endif
