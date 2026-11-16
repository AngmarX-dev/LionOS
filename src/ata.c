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
#define ATA_CMD_IDENTIFY 0xECu
#define ATA_CMD_READ    0x20u
#define ATA_CMD_WRITE   0x30u
#define ATA_SR_BSY      0x80u
#define ATA_SR_DRQ      0x08u
#define ATA_SR_ERR      0x01u
#define ATA_SR_DF       0x20u
#define ATA_TIMEOUT     1000000u

static uint32_t identified_sector_count;
static volatile uint32_t ata_io_lock;

static void ata_lock(void) {
    while (__sync_lock_test_and_set(&ata_io_lock, 1u) != 0u)
        __asm__ volatile ("pause");
}

static void ata_unlock(void) {
    __sync_lock_release(&ata_io_lock);
}

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
        if (status == 0u || status == 0xFFu) return -1;
        if (!(status & ATA_SR_BSY))
            return (status & (ATA_SR_ERR | ATA_SR_DF)) ? -1 : 0;
    }
    return -1;
}

static int wait_drq(void) {
    for (uint32_t i = 0; i < ATA_TIMEOUT; ++i) {
        uint8_t status = ata_inb(ATA_STATUS);
        if (status == 0u || status == 0xFFu) return -1;
        if (status & (ATA_SR_ERR | ATA_SR_DF)) return -1;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) return 0;
    }
    return -1;
}

static void ata_400ns_delay(void) {
    for (volatile uint32_t i = 0; i < 4u; ++i)
        (void)ata_inb(ATA_STATUS);
}

static int identify_primary_master(void) {
    uint16_t words[256];
    uint32_t sectors = 0u;
    uint8_t status;

    ata_outb(ATA_HDDEVSEL, 0xA0u);
    ata_400ns_delay();
    ata_outb(ATA_SECCOUNT0, 0u);
    ata_outb(ATA_LBA0, 0u);
    ata_outb(ATA_LBA1, 0u);
    ata_outb(ATA_LBA2, 0u);
    ata_outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    status = ata_inb(ATA_STATUS);
    if (status == 0u || status == 0xFFu)
        return -1;
    if (wait_not_busy() < 0)
        return -1;

    /* A nonzero signature in these registers identifies a non-ATA device. */
    if (ata_inb(ATA_LBA1) != 0u || ata_inb(ATA_LBA2) != 0u)
        return -1;
    if (wait_drq() < 0)
        return -1;

    for (uint32_t i = 0u; i < 256u; ++i)
        words[i] = ata_inw(ATA_DATA);
    (void)ata_inb(ATA_STATUS);

    if (ata_identify_parse(words, &sectors) < 0)
        return -1;
    identified_sector_count = sectors;
    return 0;
}

static void select_lba(uint32_t lba) {
    ata_outb(ATA_HDDEVSEL, (uint8_t)(0xE0u | ((lba >> 24) & 0x0Fu)));
    ata_400ns_delay();
    ata_outb(ATA_SECCOUNT0, 1u);
    ata_outb(ATA_LBA0, (uint8_t)lba);
    ata_outb(ATA_LBA1, (uint8_t)(lba >> 8));
    ata_outb(ATA_LBA2, (uint8_t)(lba >> 16));
}

int ata_init(void) {
    int result;

    ata_lock();
    identified_sector_count = 0u;
    result = identify_primary_master();
    ata_unlock();
    return result;
}

uint32_t ata_sector_count(void) {
    return identified_sector_count;
}

int ata_read_sector(uint32_t lba, void *buffer) {
    uint8_t *dst = (uint8_t *)buffer;
    if (!buffer || identified_sector_count == 0u ||
        lba >= identified_sector_count)
        return -1;

    ata_lock();
    select_lba(lba);
    ata_outb(ATA_COMMAND, ATA_CMD_READ);
    if (wait_drq() < 0) {
        ata_unlock();
        return -1;
    }

    for (uint32_t i = 0u; i < 256u; ++i) {
        uint16_t word = ata_inw(ATA_DATA);
        dst[i * 2u] = (uint8_t)word;
        dst[i * 2u + 1u] = (uint8_t)(word >> 8);
    }
    (void)ata_inb(ATA_STATUS);
    ata_unlock();
    return 0;
}

int ata_write_sector(uint32_t lba, const void *buffer) {
    const uint8_t *src = (const uint8_t *)buffer;
    if (!buffer || identified_sector_count == 0u ||
        lba >= identified_sector_count)
        return -1;

    ata_lock();
    select_lba(lba);
    ata_outb(ATA_COMMAND, ATA_CMD_WRITE);
    if (wait_drq() < 0) {
        ata_unlock();
        return -1;
    }

    for (uint32_t i = 0u; i < 256u; ++i) {
        uint16_t word = (uint16_t)src[i * 2u] |
                        ((uint16_t)src[i * 2u + 1u] << 8);
        ata_outw(ATA_DATA, word);
    }
    int result = wait_not_busy();
    ata_unlock();
    return result;
}
