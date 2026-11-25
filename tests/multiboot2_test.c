#include <stdint.h>
#include <stdio.h>
#include "multiboot2.h"

static uint8_t storage[256] __attribute__((aligned(8)));

static void put32(uint32_t offset,uint32_t value)
{
    *(uint32_t *)(void *)(storage+offset)=value;
}

static void make_valid(void)
{
    for(uint32_t i=0u;i<sizeof(storage);++i)storage[i]=0u;
    put32(0u,56u);
    put32(4u,0u);
    put32(8u,MULTIBOOT2_TAG_MMAP);
    put32(12u,40u);
    put32(16u,24u);
    put32(20u,0u);
    *(uint64_t *)(void *)(storage+24u)=0x100000u;
    *(uint64_t *)(void *)(storage+32u)=0x400000u;
    put32(40u,MULTIBOOT2_MEMORY_AVAILABLE);
    put32(44u,0u);
    put32(48u,MULTIBOOT2_TAG_END);
    put32(52u,8u);
}

static void make_valid_module(void)
{
    make_valid();
    put32(0u,80u);
    put32(48u,MULTIBOOT2_TAG_MODULE);
    put32(52u,18u);
    put32(56u,0x200000u);
    put32(60u,0x201000u);
    storage[64u]='m';
    storage[65u]=0u;
    put32(72u,MULTIBOOT2_TAG_END);
    put32(76u,8u);
}

static int expect_invalid(int code)
{
    if(multiboot2_info_validate(storage)==0){
        printf("Multiboot2 validation test failed: %d\n",code);
        return code;
    }
    return 0;
}

int main(void)
{
    make_valid();
    if(multiboot2_info_validate(storage)!=0)return 1;

    mb2_info_iterator_t iterator;
    mb2_tag_view_t tag;
    if(multiboot2_info_iterator_init(storage,&iterator)!=0||
       multiboot2_info_iterator_next(&iterator,&tag)!=1||
       tag.type!=MULTIBOOT2_TAG_MMAP||tag.size!=40u||
       multiboot2_info_iterator_next(&iterator,&tag)!=0)return 2;

    put32(0u,8u);if(expect_invalid(3))return 3;
    make_valid();put32(4u,1u);if(expect_invalid(4))return 4;
    make_valid();put32(0u,57u);if(expect_invalid(5))return 5;
    make_valid();put32(12u,7u);if(expect_invalid(6))return 6;
    make_valid();put32(12u,48u);if(expect_invalid(7))return 7;
    make_valid();put32(16u,23u);if(expect_invalid(8))return 8;
    make_valid();put32(20u,1u);if(expect_invalid(9))return 9;
    make_valid();put32(12u,39u);if(expect_invalid(10))return 10;
    make_valid();put32(48u,1u);if(expect_invalid(11))return 11;
    make_valid();put32(52u,7u);if(expect_invalid(12))return 12;
    make_valid();put32(52u,16u);if(expect_invalid(13))return 13;
    make_valid();put32(0u,48u);if(expect_invalid(14))return 14;
    make_valid();put32(16u,24u);put32(12u,32u);if(expect_invalid(15))return 15;
    make_valid();put32(8u,99u);if(expect_invalid(16))return 16;
    make_valid();put32(8u,3u);put32(12u,12u);if(expect_invalid(17))return 17;

    make_valid();
    put32(8u,MULTIBOOT2_TAG_MMAP);put32(12u,40u);
    put32(48u,MULTIBOOT2_TAG_MMAP);put32(52u,40u);
    put32(56u,24u);put32(60u,0u);
    *(uint64_t *)(void *)(storage+64u)=0x500000u;
    *(uint64_t *)(void *)(storage+72u)=0x100000u;
    put32(80u,MULTIBOOT2_MEMORY_AVAILABLE);put32(84u,0u);
    put32(88u,MULTIBOOT2_TAG_END);put32(92u,8u);
    put32(0u,96u);
    if(expect_invalid(18))return 18;

    make_valid_module();
    put32(56u,0x201000u);put32(60u,0x200000u);
    if(expect_invalid(19))return 19;

    make_valid_module();
    storage[65u]='x';
    if(expect_invalid(23))return 23;

    make_valid();
    put32(16u,25u);
    put32(12u,41u);
    put32(56u,MULTIBOOT2_TAG_END);
    put32(60u,8u);
    put32(0u,64u);
    if(expect_invalid(24))return 24;

    make_valid();
    *(uint64_t *)(void *)(storage+24u)=UINT64_MAX-0x7ffu;
    *(uint64_t *)(void *)(storage+32u)=0x1000u;
    if(expect_invalid(22))return 22;

    make_valid();
    if(multiboot2_info_iterator_init(storage+1u,&iterator)==0)return 20;
    if(multiboot2_info_iterator_init(storage,0)==0)return 21;

    /* Public iterator state must remain aligned and bounded on every step. */
    make_valid();
    if (multiboot2_info_iterator_init(storage, &iterator) != 0)
        return 25;
    mb2_info_iterator_t corrupted = iterator;
    corrupted.offset = 9u;
    if (multiboot2_info_iterator_next(&corrupted, &tag) != -1)
        return 26;
    corrupted = iterator;
    corrupted.offset = 4u;
    if (multiboot2_info_iterator_next(&corrupted, &tag) != -1)
        return 27;
    corrupted = iterator;
    corrupted.total_size = 8u;
    if (multiboot2_info_iterator_next(&corrupted, &tag) != -1)
        return 28;
    corrupted = iterator;
    corrupted.total_size = 57u;
    if (multiboot2_info_iterator_next(&corrupted, &tag) != -1)
        return 29;
    corrupted = iterator;
    corrupted.base = storage + 1u;
    if (multiboot2_info_iterator_next(&corrupted, &tag) != -1)
        return 30;
    corrupted = iterator;
    corrupted.base = (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 7u);
    if (multiboot2_info_iterator_next(&corrupted, &tag) != -1)
        return 31;
    corrupted = iterator;
    corrupted.ended = 2u;
    if (multiboot2_info_iterator_next(&corrupted, &tag) != -1)
        return 32;

    puts("Multiboot2 bounded tag and memory-map validation tests passed");
    return 0;
}
