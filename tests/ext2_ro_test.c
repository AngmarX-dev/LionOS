#include <stdint.h>
#include <stdio.h>
#include "block_device.h"
#include "ext2_ro.h"

#define IMAGE_BLOCKS 32u
#define BLOCK_SIZE 1024u
#define IMAGE_SECTORS (IMAGE_BLOCKS * 2u)

struct disk_image { uint8_t bytes[IMAGE_BLOCKS * BLOCK_SIZE]; };

static uint16_t get16(uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static void put16(uint8_t *p, uint16_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8);
}
static void put32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8);
    p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static int image_read(void *context, uint64_t first, uint32_t count, void *buffer) {
    struct disk_image *image=(struct disk_image*)context;
    uint64_t offset=first*512u, bytes=(uint64_t)count*512u;
    if(!image||!buffer||offset+bytes>sizeof(image->bytes))return -1;
    for(uint32_t i=0;i<(uint32_t)bytes;++i)
        ((uint8_t*)buffer)[i]=image->bytes[(uint32_t)offset+i];
    return 0;
}
static void make_image(struct disk_image *image) {
    uint8_t *sb=image->bytes+1024u;
    uint8_t *gd=image->bytes+2u*BLOCK_SIZE;
    uint8_t *itable=image->bytes+5u*BLOCK_SIZE;
    uint8_t *root=image->bytes+6u*BLOCK_SIZE;
    uint8_t *data=image->bytes+7u*BLOCK_SIZE;
    uint8_t *indirect=image->bytes+8u*BLOCK_SIZE;
    uint8_t *indirect_data=image->bytes+9u*BLOCK_SIZE;

    for(uint32_t i=0;i<sizeof(image->bytes);++i)image->bytes[i]=0;
    put32(sb+0u,16u); put32(sb+4u,IMAGE_BLOCKS);
    put32(sb+20u,1u); put32(sb+24u,0u);
    put32(sb+32u,31u); put32(sb+40u,16u);
    put32(sb+76u,1u); put16(sb+88u,128u);
    put32(sb+92u,0u); put32(sb+96u,2u); put32(sb+100u,1u);
    put16(sb+56u,0xef53u);
    put32(gd+8u,5u);

    /* inode 2: root directory */
    put16(itable+128u,0x41edu); put32(itable+128u+4u,BLOCK_SIZE);
    put32(itable+128u+40u,6u);
    /* inode 3: regular file containing "hello" */
    put16(itable+256u,0x81a4u); put32(itable+256u+4u,5u);
    put32(itable+256u+40u,7u);
    /* inode 4: exercises one singly-indirect lookup */
    put16(itable+384u,0x81a4u); put32(itable+384u+4u,13u*BLOCK_SIZE+3u);
    put32(itable+384u+40u+12u*4u,8u);
    put32(root+0u,2u); put16(root+4u,12u); root[6]=1u; root[8]='.';
    put32(root+12u,2u); put16(root+16u,12u); root[18]=2u;
    root[20]='.'; root[21]='.';
    put32(root+24u,3u); put16(root+28u,16u); root[30]=5u;
    root[32]='h'; root[33]='e'; root[34]='l'; root[35]='l'; root[36]='o';
    put16(root+40u, BLOCK_SIZE-40u); /* final unused record */
    data[0]='h'; data[1]='e'; data[2]='l'; data[3]='l'; data[4]='o';
    put32(indirect,9u);
    indirect_data[0]='I'; indirect_data[1]='N'; indirect_data[2]='D';
}

static int expect(int ok,const char *label) {
    if(!ok){(void)fprintf(stderr,"FAIL: %s\n",label);return 1;}
    return 0;
}
int main(void) {
    static const struct block_device_ops ops={image_read};
    struct disk_image image;
    struct block_device dev={&ops,&image,IMAGE_SECTORS,512u};
    struct ext2_ro_fs fs;
    struct ext2_ro_inode root,file,large;
    uint8_t scratch[EXT2_RO_BLOCK_MAX];
    uint8_t output[8]={0};
    uint32_t found=0u;
    int failures=0;
    make_image(&image);
    failures+=expect(ext2_ro_mount(&fs,&dev,scratch,sizeof(scratch))==0,
                     "mount valid ext2 superblock");
    failures+=expect(ext2_ro_read_inode(&fs,2u,&root,scratch,sizeof(scratch))==0,
                     "read root inode");
    failures+=expect(ext2_ro_lookup(&fs,&root,"hello",&found,scratch,sizeof(scratch))==0 &&
                     found==3u,"lookup regular file in directory");
    failures+=expect(ext2_ro_read_inode(&fs,3u,&file,scratch,sizeof(scratch))==0,
                     "read regular inode");
    failures+=expect(ext2_ro_read_file(&fs,&file,0u,output,5u,scratch,sizeof(scratch))==5 &&
                     output[0]=='h'&&output[4]=='o',"read direct data block");
    failures+=expect(ext2_ro_read_inode(&fs,4u,&large,scratch,sizeof(scratch))==0,
                     "read singly-indirect test inode");
    failures+=expect(ext2_ro_read_file(&fs,&large,12u*BLOCK_SIZE,output,3u,
                                      scratch,sizeof(scratch))==3 &&
                     output[0]=='I'&&output[2]=='D',"read singly-indirect data block");

    /* Malformed directory records must fail closed. */
    put16(image.bytes+6u*BLOCK_SIZE+4u,0u);
    failures+=expect(ext2_ro_lookup(&fs,&root,"missing",&found,scratch,sizeof(scratch))<0,
                     "reject zero directory record length");
    make_image(&image);
    put32(image.bytes+1024u+96u,0x80000000u);
    failures+=expect(ext2_ro_mount(&fs,&dev,scratch,sizeof(scratch))<0,
                     "reject unsupported incompat feature");
    make_image(&image);
    put32(image.bytes+1024u+4u,IMAGE_BLOCKS+1u);
    failures+=expect(ext2_ro_mount(&fs,&dev,scratch,sizeof(scratch))<0,
                     "reject filesystem larger than device");
    make_image(&image);
    put16(image.bytes+1024u+56u,0u);
    failures+=expect(ext2_ro_mount(&fs,&dev,scratch,sizeof(scratch))<0,
                     "reject invalid ext2 magic");

    if(failures)return 1;
    (void)puts("EXT2 READ-ONLY PARSER TEST: PASS");
    return 0;
}
