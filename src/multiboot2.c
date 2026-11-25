#include <stdint.h>
#include "multiboot2.h"

#define MB2_HEADER_SIZE 8u
#define MB2_TAG_HEADER_SIZE 8u
#define MB2_MMAP_HEADER_SIZE 16u
#define MB2_MMAP_ENTRY_MIN_SIZE 24u
#define MB2_END_TAG_SIZE 8u

int multiboot2_info_iterator_init(const void *info, mb2_info_iterator_t *iterator)
{
    uintptr_t address=(uintptr_t)info;
    if(!info||!iterator||(address&7u)!=0u||
       address>UINTPTR_MAX-MB2_HEADER_SIZE)
        return -1;

    const uint8_t *bytes=(const uint8_t *)info;
    uint32_t total_size=*(const uint32_t *)(const void *)bytes;
    uint32_t reserved=*(const uint32_t *)(const void *)(bytes+4u);
    if(reserved!=0u||total_size<16u||(total_size&7u)!=0u||
       address>UINTPTR_MAX-total_size)
        return -1;

    iterator->base=bytes;
    iterator->total_size=total_size;
    iterator->offset=MB2_HEADER_SIZE;
    iterator->ended=0u;
    return 0;
}

static int iterator_state_valid(const mb2_info_iterator_t *iterator)
{
    uintptr_t address;

    if (!iterator || !iterator->base || iterator->ended > 1u)
        return 0;

    address = (uintptr_t)iterator->base;
    if ((address & 7u) != 0u ||
        iterator->total_size < 16u ||
        (iterator->total_size & 7u) != 0u ||
        address > UINTPTR_MAX - iterator->total_size ||
        iterator->offset < MB2_HEADER_SIZE ||
        (iterator->offset & 7u) != 0u ||
        iterator->offset > iterator->total_size)
        return 0;

    return 1;
}

int multiboot2_info_iterator_next(mb2_info_iterator_t *iterator, mb2_tag_view_t *tag)
{
    /*
     * The iterator structure is public. Revalidate its cursor invariants on
     * every call so accidental or corrupted caller state cannot cause
     * unaligned reads or pointer arithmetic outside the validated block.
     */
    if (!tag || !iterator_state_valid(iterator) || iterator->ended != 0u)
        return -1;
    if(iterator->offset==iterator->total_size)return -1;
    if(iterator->total_size-iterator->offset<MB2_TAG_HEADER_SIZE)
        return -1;

    const uint8_t *current=iterator->base+iterator->offset;
    uint32_t type=*(const uint32_t *)(const void *)current;
    uint32_t size=*(const uint32_t *)(const void *)(current+4u);
    if(size<MB2_TAG_HEADER_SIZE||size>iterator->total_size-iterator->offset)
        return -1;

    uint64_t aligned=((uint64_t)size+7u)&~7ULL;
    if(aligned>iterator->total_size-iterator->offset)
        return -1;

    if(type==MULTIBOOT2_TAG_END){
        if(size!=MB2_END_TAG_SIZE||
           iterator->offset+MB2_END_TAG_SIZE!=iterator->total_size)
            return -1;
        iterator->ended=1u;
        tag->type=type;
        tag->size=size;
        tag->payload=current+MB2_TAG_HEADER_SIZE;
        return 0;
    }

    tag->type=type;
    tag->size=size;
    tag->payload=current+MB2_TAG_HEADER_SIZE;
    iterator->offset+=(uint32_t)aligned;
    return 1;
}

static int mmap_tag_valid(const mb2_tag_view_t *tag)
{
    if(!tag||tag->type!=MULTIBOOT2_TAG_MMAP||tag->size<MB2_MMAP_HEADER_SIZE)
        return 0;
    uint32_t entry_size=*(const uint32_t *)(const void *)tag->payload;
    uint32_t entry_version=*(const uint32_t *)(const void *)(tag->payload+4u);
    uint32_t entries_size=tag->size-MB2_MMAP_HEADER_SIZE;
    if(entry_size<MB2_MMAP_ENTRY_MIN_SIZE||(entry_size&7u)!=0u||
       entry_version!=0u||entries_size%entry_size!=0u)
        return 0;

    /*
     * Reject overflowing physical ranges during validation, before the
     * allocator sees any firmware-provided interval.
     */
    for (uint32_t offset = 0u; offset < entries_size; offset += entry_size) {
        /* The tag payload starts after the 8-byte generic tag header. */
        const uint8_t *entry = tag->payload + 8u + offset;
        uint64_t address = *(const uint64_t *)(const void *)entry;
        uint64_t length = *(const uint64_t *)(const void *)(entry + 8u);
        if (length > UINT64_MAX - address)
            return 0;
    }
    return 1;
}

int multiboot2_info_validate(const void *info)
{
    mb2_info_iterator_t iterator;
    mb2_tag_view_t tag;
    uint32_t mmap_count=0u;
    if(multiboot2_info_iterator_init(info,&iterator)!=0)return -1;

    for(;;){
        int result=multiboot2_info_iterator_next(&iterator,&tag);
        if(result<0)return -1;
        if(result==0)break;
        if(tag.type==MULTIBOOT2_TAG_MMAP){
            if(!mmap_tag_valid(&tag)||++mmap_count!=1u)return -1;
        }else if(tag.type==MULTIBOOT2_TAG_MODULE){
            if(tag.size<17u)return -1;
            uint32_t module_start=*(const uint32_t *)(const void *)tag.payload;
            uint32_t module_end=*(const uint32_t *)(const void *)(tag.payload+4u);
            if(module_end<=module_start)return -1;

            /*
             * The module command line is a bounded, NUL-terminated string.
             * Never allow consumers to scan beyond the tag payload.
             */
            const uint8_t *command_line=tag.payload+8u;
            uint32_t command_line_size=tag.size-16u;
            int terminated=0;
            for(uint32_t i=0u;i<command_line_size;++i){
                if(command_line[i]==0u){
                    terminated=1;
                    break;
                }
            }
            if(!terminated)return -1;
        }
    }
    return mmap_count==1u?0:-1;
}
