#include <stdint.h>
#include <stdio.h>
#include "ata.h"

static int expect(int condition, const char *name) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        return 1;
    }
    return 0;
}

int main(void) {
    uint16_t words[256] = {0};
    uint32_t sectors = 0u;
    int failed = 0;

    words[49] = 1u << 9;
    words[60] = 16384u;
    failed += expect(ata_identify_parse(words, &sectors) == 0 &&
                     sectors == 16384u, "valid LBA28 capacity");

    words[60] = 0u;
    words[61] = 0u;
    failed += expect(ata_identify_parse(words, &sectors) < 0,
                     "zero capacity rejected");

    words[49] = 0u;
    words[60] = 16384u;
    failed += expect(ata_identify_parse(words, &sectors) < 0,
                     "device without LBA rejected");

    words[49] = 1u << 9;
    words[60] = 0xffffu;
    words[61] = 0xffffu;
    failed += expect(ata_identify_parse(words, &sectors) == 0 &&
                     sectors == 0x10000000u,
                     "capacity capped to LBA28 address space");

    failed += expect(ata_identify_parse(words, 0) < 0,
                     "null output rejected");
    failed += expect(ata_identify_parse(0, &sectors) < 0,
                     "null identify data rejected");

    if (failed)
        return 1;
    puts("ATA IDENTIFY capacity parser: PASS");
    return 0;
}
