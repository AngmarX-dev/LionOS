#ifndef LIONOS_ATA_H
#define LIONOS_ATA_H

#include <stdint.h>

int ata_init(void);
uint32_t ata_sector_count(void);
int ata_slave_init(void);
uint32_t ata_slave_sector_count(void);
int ata_identify_parse(const uint16_t words[256], uint32_t *sector_count);
int ata_read_sector(uint32_t lba, void *buffer);
int ata_write_sector(uint32_t lba, const void *buffer);
int ata_slave_read_sector(uint32_t lba, void *buffer);

#endif
