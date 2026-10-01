#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>

typedef struct {
    uint32_t controller_found;
    uint32_t initialized;
    uint32_t ready;
    uint32_t port;
    uint32_t speed;
    uint32_t slot;
    uint32_t endpoint_id;
    uint32_t endpoint_address;
    uint32_t packet_size;
    uint32_t interval;
    uint16_t vid;
    uint16_t pid;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    uint8_t  hid_iface;
    uint8_t  hid_subclass;
    uint8_t  hid_protocol;
    uint8_t  hid_endpoint;
    uint8_t  hid_ep_type;
    uint16_t hid_packet;
    uint8_t  hid_interval;
    uint32_t interfaces;
    uint32_t endpoints;
    uint32_t hid_interfaces;
    uint32_t submitted;
    uint32_t events;
    uint32_t successes;
    uint32_t errors;
    uint32_t reports;
    uint32_t last_completion;
    uint32_t portsc;
    const char *stage;
    uint32_t report_len;
    uint8_t  report[8];
    uint32_t usb_status;
    uint32_t usb_command;
} xhci_mouse_debug_info_t;

int xhci_mouse_init(void);
int xhci_mouse_poll(int32_t *dx, int32_t *dy, uint8_t *buttons);
int xhci_mouse_recover(void);
int xhci_mouse_debug_get(xhci_mouse_debug_info_t *out);

#endif
