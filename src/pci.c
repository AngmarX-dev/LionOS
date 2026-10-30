#include <stdint.h>
#include "pci.h"
#include "console.h"

#define PCI_CONFIG_ADDRESS 0x0CF8u
#define PCI_CONFIG_DATA    0x0CFCu
#define PCI_MAX_RECORDED   128u
#define PCI_PRINT_LIMIT    16u

typedef struct {
    uint8_t bus, device, function;
    uint16_t vendor, product;
    uint8_t class_code, subclass, prog_if, revision;
    uint8_t header_type;
} pci_device_t;

static pci_device_t devices[PCI_MAX_RECORDED];
static uint32_t discovered;
static uint32_t recorded;
static uint32_t initialized;

static inline void pci_outl(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t pci_inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t pci_read32(uint8_t bus, uint8_t device,
                           uint8_t function, uint8_t offset) {
    uint32_t address = 0x80000000u
        | ((uint32_t)bus << 16)
        | ((uint32_t)device << 11)
        | ((uint32_t)function << 8)
        | ((uint32_t)offset & 0xFCu);
    pci_outl(PCI_CONFIG_ADDRESS, address);
    return pci_inl(PCI_CONFIG_DATA);
}

static uint16_t pci_vendor(uint8_t bus, uint8_t device, uint8_t function) {
    return (uint16_t)(pci_read32(bus, device, function, 0u) & 0xFFFFu);
}

static void pci_record(uint8_t bus, uint8_t device, uint8_t function) {
    uint32_t id = pci_read32(bus, device, function, 0u);
    uint16_t vendor = (uint16_t)id;
    if (vendor == 0xFFFFu) return;

    ++discovered;
    if (recorded >= PCI_MAX_RECORDED) return;

    uint32_t class_reg = pci_read32(bus, device, function, 8u);
    uint32_t header_reg = pci_read32(bus, device, function, 0x0Cu);
    pci_device_t *d = &devices[recorded++];

    d->bus = bus;
    d->device = device;
    d->function = function;
    d->vendor = vendor;
    d->product = (uint16_t)(id >> 16);
    d->revision = (uint8_t)class_reg;
    d->prog_if = (uint8_t)(class_reg >> 8);
    d->subclass = (uint8_t)(class_reg >> 16);
    d->class_code = (uint8_t)(class_reg >> 24);
    d->header_type = (uint8_t)(header_reg >> 16);
}

int pci_init(void) {
    discovered = 0u;
    recorded = 0u;
    initialized = 0u;

    for (uint32_t bus = 0; bus < 256u; ++bus) {
        for (uint32_t dev = 0; dev < 32u; ++dev) {
            uint16_t vendor = pci_vendor((uint8_t)bus, (uint8_t)dev, 0u);
            if (vendor == 0xFFFFu) continue;

            pci_record((uint8_t)bus, (uint8_t)dev, 0u);
            uint8_t header = (uint8_t)(pci_read32((uint8_t)bus,
                (uint8_t)dev, 0u, 0x0Cu) >> 16);
            if (header & 0x80u) {
                for (uint32_t fn = 1u; fn < 8u; ++fn) {
                    if (pci_vendor((uint8_t)bus, (uint8_t)dev,
                                   (uint8_t)fn) != 0xFFFFu)
                        pci_record((uint8_t)bus, (uint8_t)dev, (uint8_t)fn);
                }
            }
        }
    }

    initialized = 1u;
    return 0;
}

uint32_t pci_device_count(void) {
    return discovered;
}

void pci_print_devices(void) {
    if (!initialized) {
        console_write("[ -- ] PCI enumeration has not run\n");
        return;
    }

    console_write("[ OK ] PCI configuration-space scan complete\n");
    console_write("       Devices found: ");
    console_write_dec(discovered);
    console_write(" / recorded: ");
    console_write_dec(recorded);
    console_write("\n");

    uint32_t limit = recorded < PCI_PRINT_LIMIT ? recorded : PCI_PRINT_LIMIT;
    for (uint32_t i = 0; i < limit; ++i) {
        const pci_device_t *d = &devices[i];
        console_write("       ");
        console_write_dec(d->bus);
        console_putc(':');
        console_write_dec(d->device);
        console_putc('.');
        console_write_dec(d->function);
        console_write("  vendor=");
        console_write_hex(d->vendor);
        console_write(" device=");
        console_write_hex(d->product);
        console_write(" class=");
        console_write_hex(d->class_code);
        console_putc(':');
        console_write_hex(d->subclass);
        console_write(" if=");
        console_write_hex(d->prog_if);
        console_write("\n");
    }

    if (discovered > limit)
        console_write("       (additional devices omitted from boot summary)\n");
    if (discovered > recorded)
        console_write("       WARNING: PCI device storage limit reached\n");
}
