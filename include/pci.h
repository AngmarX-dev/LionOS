#ifndef LIONOS_PCI_H
#define LIONOS_PCI_H

#include <stdint.h>

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t programming_interface;
    uint8_t revision;
} pci_device_info_t;

int pci_init(void);
int pci_is_initialized(void);
uint32_t pci_device_count(void);
uint32_t pci_recorded_device_count(void);
int pci_get_device(uint32_t index, pci_device_info_t *out);
void pci_print_devices(void);

#endif
