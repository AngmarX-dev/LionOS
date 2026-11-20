#ifndef LIONOS_MULTIBOOT2_H
#define LIONOS_MULTIBOOT2_H

#include <stdint.h>

#define MULTIBOOT2_TAG_END 0u
#define MULTIBOOT2_TAG_MODULE 3u
#define MULTIBOOT2_TAG_MMAP 6u
#define MULTIBOOT2_MEMORY_AVAILABLE 1u

typedef struct mb2_info_iterator {
    const uint8_t *base;
    uint32_t total_size;
    uint32_t offset;
    uint32_t ended;
} mb2_info_iterator_t;

typedef struct mb2_tag_view {
    uint32_t type;
    uint32_t size;
    const uint8_t *payload;
} mb2_tag_view_t;

/* Validate the entire bounded information block and all known variable-size tags. */
int multiboot2_info_validate(const void *info);
/* Iterator returns 1 for a tag, 0 for the terminal end tag, and -1 on malformed input. */
int multiboot2_info_iterator_init(const void *info, mb2_info_iterator_t *iterator);
int multiboot2_info_iterator_next(mb2_info_iterator_t *iterator, mb2_tag_view_t *tag);

#endif
