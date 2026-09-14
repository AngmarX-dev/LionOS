#include <stdint.h>
#include "ata.h"
#include "diskfs.h"

#define DISKFS_MAGIC0 'L'
#define DISKFS_MAGIC1 'I'
#define DISKFS_MAGIC2 'O'
#define DISKFS_MAGIC3 'N'
#define DISKFS_MAGIC4 'F'
#define DISKFS_MAGIC5 'S'
#define DISKFS_MAGIC6 '1'
#define DISKFS_VERSION 1u
#define DISKFS_SUPER_LBA 0u
#define DISKFS_DIR_LBA 1u
#define DISKFS_DIR_SECTORS 2u
#define DISKFS_DATA_LBA 3u
#define DISKFS_SECTORS_PER_FILE 8u
#define ATA_SECTOR_SIZE 512u

struct diskfs_entry { char name[DISKFS_NAME_MAX]; uint32_t size; uint32_t start_lba; };
static struct diskfs_entry entries[DISKFS_MAX_FILES];
static uint8_t sector[ATA_SECTOR_SIZE];
static uint32_t file_count;
static int available;

static void zero(void *buffer, uint32_t size) { uint8_t *p=(uint8_t*)buffer; for(uint32_t i=0;i<size;++i)p[i]=0; }
static int streq(const char *a,const char *b){if(!a||!b)return 0;while(*a&&*a==*b){++a;++b;}return *a==*b;}
static void copy_name(char *dst,const char *src){uint32_t i=0;while(src[i]&&i<DISKFS_NAME_MAX-1u){dst[i]=src[i];++i;}dst[i]=0;}
static int valid_name(const char *name){if(!name||!*name)return 0;uint32_t n=0;while(name[n]){if(name[n]=='/'||name[n]=='\\')return 0;if(++n>=DISKFS_NAME_MAX)return 0;}return 1;}
static int find(const char *name){for(uint32_t i=0;i<DISKFS_MAX_FILES;++i)if(entries[i].name[0]&&streq(entries[i].name,name))return(int)i;return-1;}
static int write_directory_sector(uint32_t s){zero(sector,ATA_SECTOR_SIZE);const uint8_t *src=(const uint8_t*)entries+s*ATA_SECTOR_SIZE;for(uint32_t i=0;i<ATA_SECTOR_SIZE;++i)sector[i]=src[i];return ata_write_sector(DISKFS_DIR_LBA+s,sector);}
static int save_super(void){zero(sector,ATA_SECTOR_SIZE);sector[0]='L';sector[1]='I';sector[2]='O';sector[3]='N';sector[4]='F';sector[5]='S';sector[6]='1';*(uint32_t*)&sector[8]=DISKFS_VERSION;*(uint32_t*)&sector[12]=file_count;return ata_write_sector(DISKFS_SUPER_LBA,sector);}
static int format(void){zero(entries,sizeof(entries));file_count=0;if(save_super()<0)return-1;for(uint32_t i=0;i<DISKFS_DIR_SECTORS;++i)if(write_directory_sector(i)<0)return-1;return 0;}

int diskfs_init(void){available=0;file_count=0;zero(entries,sizeof(entries));if(ata_init()<0)return-1;if(ata_read_sector(DISKFS_SUPER_LBA,sector)<0)return-1;if(sector[0]!='L'||sector[1]!='I'||sector[2]!='O'||sector[3]!='N'||sector[4]!='F'||sector[5]!='S'||sector[6]!='1'||*(uint32_t*)&sector[8]!=DISKFS_VERSION){if(format()<0)return-1;}else{file_count=*(uint32_t*)&sector[12];if(file_count>DISKFS_MAX_FILES)return-1;for(uint32_t i=0;i<DISKFS_DIR_SECTORS;++i)if(ata_read_sector(DISKFS_DIR_LBA+i,sector)<0)return-1;else{uint8_t *dst=(uint8_t*)entries+i*ATA_SECTOR_SIZE;for(uint32_t j=0;j<ATA_SECTOR_SIZE;++j)dst[j]=sector[j];}}uint32_t actual=0;for(uint32_t i=0;i<DISKFS_MAX_FILES;++i){if(!entries[i].name[0])continue;if(entries[i].size>DISKFS_MAX_FILE_SIZE||entries[i].start_lba!=DISKFS_DATA_LBA+i*DISKFS_SECTORS_PER_FILE)entries[i].name[0]=0;else ++actual;}file_count=actual;available=1;return 0;}
int diskfs_available(void){return available;}
uint32_t diskfs_count(void){return available?file_count:0;}
const char *diskfs_name(uint32_t index){if(!available||index>=DISKFS_MAX_FILES||!entries[index].name[0])return 0;return entries[index].name;}
int diskfs_exists(const char *name){return available&&find(name)>=0;}
uint32_t diskfs_size(const char *name){int i=find(name);return i<0?0u:entries[i].size;}
int diskfs_read(const char *name,void *buffer,uint32_t capacity){if(!available||!buffer)return-1;int index=find(name);if(index<0||capacity<entries[index].size)return-1;uint8_t *dst=(uint8_t*)buffer;uint32_t remaining=entries[index].size;for(uint32_t s=0;remaining;++s){if(ata_read_sector(entries[index].start_lba+s,sector)<0)return-1;uint32_t n=remaining>ATA_SECTOR_SIZE?ATA_SECTOR_SIZE:remaining;for(uint32_t i=0;i<n;++i)dst[i]=sector[i];dst+=n;remaining-=n;}return(int)entries[index].size;}
int diskfs_write(const char *name,const void *data,uint32_t size){if(!available||!valid_name(name)||!data||size>DISKFS_MAX_FILE_SIZE)return-1;int index=find(name);if(index<0){for(uint32_t i=0;i<DISKFS_MAX_FILES;++i)if(!entries[i].name[0]){index=(int)i;break;}if(index<0)return-1;copy_name(entries[index].name,name);entries[index].start_lba=DISKFS_DATA_LBA+(uint32_t)index*DISKFS_SECTORS_PER_FILE;++file_count;}const uint8_t *src=(const uint8_t*)data;uint32_t remaining=size;for(uint32_t s=0;s<DISKFS_SECTORS_PER_FILE;++s){zero(sector,ATA_SECTOR_SIZE);uint32_t n=remaining>ATA_SECTOR_SIZE?ATA_SECTOR_SIZE:remaining;for(uint32_t i=0;i<n;++i)sector[i]=src[i];if(ata_write_sector(entries[index].start_lba+s,sector)<0)return-1;if(remaining){src+=n;remaining-=n;}}entries[index].size=size;if(save_super()<0)return-1;return write_directory_sector((uint32_t)index/16u);}
int diskfs_remove(const char *name){if(!available)return-1;int index=find(name);if(index<0)return-1;zero(&entries[index],sizeof(entries[index]));if(file_count)--file_count;if(save_super()<0)return-1;return write_directory_sector((uint32_t)index/16u);}
