#ifndef LIONOS_ATA_BLOCK_H
#define LIONOS_ATA_BLOCK_H

struct block_device;

int ata_block_device_init(struct block_device *device);
int ata_slave_block_device_init(struct block_device *device);

#endif
