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
    put32(0u,96u);
    put32(88u,MULTIBOOT2_TAG_END);put32(92u,8u);
    if(expect_invalid(18))return 18;

    make_valid();put32(8u,MULTIBOOT2_TAG_MODULE);put32(12u,16u);
    put32(16u,0x200000u);put32(20u,0x100000u);
    if(expect_invalid(19))return 19;

    make_valid();
    if(multiboot2_info_iterator_init(storage+1u,&iterator)==0)return 20;
    if(multiboot2_info_iterator_init(storage,0)==0)return 21;
    puts("Multiboot2 bounded tag and memory-map validation tests passed");
    return 0;
}
