#ifndef LIONOS_PCI_H
#define LIONOS_PCI_H

#include <stdint.h>

int pci_init(void);
uint32_t pci_device_count(void);
void pci_print_devices(void);

#endif
