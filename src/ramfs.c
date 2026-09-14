#include <stdint.h>
#include "ramfs.h"

#define RAMFS_MAX_FILES 16u
#define RAMFS_NAME_MAX 16u
#define RAMFS_DATA_MAX 256u
struct ramfs_file { char name[RAMFS_NAME_MAX]; char data[RAMFS_DATA_MAX]; uint32_t size; };
static struct ramfs_file files[RAMFS_MAX_FILES]; static uint32_t count;
extern const uint8_t _binary_hello_elf_start[],_binary_hello_elf_end[];
extern const uint8_t _binary_process_test_elf_start[],_binary_process_test_elf_end[];
extern const uint8_t _binary_ipc_test_elf_start[],_binary_ipc_test_elf_end[];
extern const uint8_t _binary_signal_test_elf_start[],_binary_signal_test_elf_end[];
static int streq(const char*a,const char*b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
static struct ramfs_file*find(const char*n){for(uint32_t i=0;i<count;++i)if(streq(files[i].name,n))return &files[i];return 0;}
static void copy_name(char*d,const char*s){uint32_t i=0;while(s[i]&&i<RAMFS_NAME_MAX-1u){d[i]=s[i];++i;}d[i]=0;}
void ramfs_init(void){count=0;ramfs_write("readme.txt","Welcome to LionOS.\nBuilt as an experimental 32-bit x86 OS.\n",61u);ramfs_write("version","LionOS 0.7\n",12u);ramfs_write("motd","The LionOS kernel is alive.\n",28u);}
uint32_t ramfs_count(void){return count+4u;}
const char*ramfs_name(uint32_t i){if(i<count)return files[i].name;if(i==count)return "hello.elf";if(i==count+1u)return "process_test.elf";if(i==count+2u)return "ipc_test.elf";if(i==count+3u)return "signal_test.elf";return 0;}
const char*ramfs_data(const char*n){if(streq(n,"hello.elf"))return(const char*)_binary_hello_elf_start;if(streq(n,"process_test.elf"))return(const char*)_binary_process_test_elf_start;if(streq(n,"ipc_test.elf"))return(const char*)_binary_ipc_test_elf_start;if(streq(n,"signal_test.elf"))return(const char*)_binary_signal_test_elf_start;struct ramfs_file*f=find(n);return f?f->data:0;}
uint32_t ramfs_size(const char*n){if(streq(n,"hello.elf"))return(uint32_t)(_binary_hello_elf_end-_binary_hello_elf_start);if(streq(n,"process_test.elf"))return(uint32_t)(_binary_process_test_elf_end-_binary_process_test_elf_start);if(streq(n,"ipc_test.elf"))return(uint32_t)(_binary_ipc_test_elf_end-_binary_ipc_test_elf_start);if(streq(n,"signal_test.elf"))return(uint32_t)(_binary_signal_test_elf_end-_binary_signal_test_elf_start);struct ramfs_file*f=find(n);return f?f->size:0;}
int ramfs_exists(const char*n){return streq(n,"hello.elf")||streq(n,"process_test.elf")||streq(n,"ipc_test.elf")||streq(n,"signal_test.elf")||find(n)!=0;}
int ramfs_write(const char*n,const char*d,uint32_t s){if(!n||!*n||!d||streq(n,"hello.elf")||streq(n,"process_test.elf")||streq(n,"ipc_test.elf")||streq(n,"signal_test.elf"))return-1;struct ramfs_file*f=find(n);if(!f){if(count>=RAMFS_MAX_FILES-4u)return-1;f=&files[count++];copy_name(f->name,n);}if(s>RAMFS_DATA_MAX)s=RAMFS_DATA_MAX;for(uint32_t i=0;i<s;++i)f->data[i]=d[i];f->size=s;return 0;}
int ramfs_remove(const char*n){if(streq(n,"hello.elf")||streq(n,"process_test.elf")||streq(n,"ipc_test.elf")||streq(n,"signal_test.elf"))return-1;for(uint32_t i=0;i<count;++i)if(streq(files[i].name,n)){for(uint32_t j=i+1u;j<count;++j)files[j-1u]=files[j];--count;return 0;}return-1;}
