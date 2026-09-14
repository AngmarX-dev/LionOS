#include <stdint.h>
#include "ata.h"

#define ATA_DATA        0x1F0u
#define ATA_SECCOUNT0   0x1F2u
#define ATA_LBA0        0x1F3u
#define ATA_LBA1        0x1F4u
#define ATA_LBA2        0x1F5u
#define ATA_HDDEVSEL    0x1F6u
#define ATA_COMMAND     0x1F7u
#define ATA_STATUS      0x1F7u
#define ATA_CMD_READ    0x20u
#define ATA_CMD_WRITE   0x30u
#define ATA_SR_BSY      0x80u
#define ATA_SR_DRQ      0x08u
#define ATA_SR_ERR      0x01u
#define ATA_SR_DF       0x20u
#define ATA_TIMEOUT     1000000u

static inline void ata_outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t ata_inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void ata_outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t ata_inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static int wait_not_busy(void) {
    for (uint32_t i = 0; i < ATA_TIMEOUT; ++i) {
        uint8_t status = ata_inb(ATA_STATUS);
        if (status == 0) return -1;
        if (!(status & ATA_SR_BSY)) return (status & (ATA_SR_ERR | ATA_SR_DF)) ? -1 : 0;
    }
    return -1;
}

static int wait_drq(void) {
    for (uint32_t i = 0; i < ATA_TIMEOUT; ++i) {
        uint8_t status = ata_inb(ATA_STATUS);
        if (status == 0) return -1;
        if (status & (ATA_SR_ERR | ATA_SR_DF)) return -1;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) return 0;
    }
    return -1;
}

static void select_lba(uint32_t lba) {
    ata_outb(ATA_HDDEVSEL, (uint8_t)(0xE0u | ((lba >> 24) & 0x0Fu)));
    for (volatile uint32_t i = 0; i < 400u; ++i) (void)ata_inb(ATA_STATUS);
    ata_outb(ATA_SECCOUNT0, 1u);
    ata_outb(ATA_LBA0, (uint8_t)lba);
    ata_outb(ATA_LBA1, (uint8_t)(lba >> 8));
    ata_outb(ATA_LBA2, (uint8_t)(lba >> 16));
}

int ata_init(void) {
    ata_outb(ATA_HDDEVSEL, 0xE0u);
    return wait_not_busy() == 0 ? 0 : -1;
}

int ata_read_sector(uint32_t lba, void *buffer) {
    if (!buffer) return -1;
    select_lba(lba);
    ata_outb(ATA_COMMAND, ATA_CMD_READ);
    if (wait_drq() < 0) return -1;

    uint16_t *dst = (uint16_t *)buffer;
    for (uint32_t i = 0; i < 256u; ++i) dst[i] = ata_inw(ATA_DATA);
    (void)ata_inb(ATA_STATUS);
    return 0;
}

int ata_write_sector(uint32_t lba, const void *buffer) {
    if (!buffer) return -1;
    select_lba(lba);
    ata_outb(ATA_COMMAND, ATA_CMD_WRITE);
    if (wait_drq() < 0) return -1;

    const uint16_t *src = (const uint16_t *)buffer;
    for (uint32_t i = 0; i < 256u; ++i) ata_outw(ATA_DATA, src[i]);
    return wait_not_busy();
}
