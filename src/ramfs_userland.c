#include <stdint.h>
#include "ramfs.h"
#define RAMFS_MAX_FILES 16u
#define RAMFS_NAME_MAX 16u
#define RAMFS_DATA_MAX 256u
#define RAMFS_PROGRAM_COUNT 13u
struct ramfs_file { char name[RAMFS_NAME_MAX]; char data[RAMFS_DATA_MAX]; uint32_t size; };
static struct ramfs_file files[RAMFS_MAX_FILES]; static uint32_t count;
extern const uint8_t _binary_hello_elf_start[],_binary_hello_elf_end[];
extern const uint8_t _binary_process_test_elf_start[],_binary_process_test_elf_end[];
extern const uint8_t _binary_ipc_test_elf_start[],_binary_ipc_test_elf_end[];
extern const uint8_t _binary_signal_test_elf_start[],_binary_signal_test_elf_end[];
extern const uint8_t _binary_net_test_elf_start[],_binary_net_test_elf_end[];
extern const uint8_t _binary_echo_elf_start[],_binary_echo_elf_end[];
extern const uint8_t _binary_cat_elf_start[],_binary_cat_elf_end[];
extern const uint8_t _binary_ls_elf_start[],_binary_ls_elf_end[];
extern const uint8_t _binary_pwd_elf_start[],_binary_pwd_elf_end[];
extern const uint8_t _binary_uname_elf_start[],_binary_uname_elf_end[];
extern const uint8_t _binary_rm_elf_start[],_binary_rm_elf_end[];
extern const uint8_t _binary_stat_elf_start[],_binary_stat_elf_end[];
extern const uint8_t _binary_userland_test_elf_start[],_binary_userland_test_elf_end[];
static const char *program_names[RAMFS_PROGRAM_COUNT]={"hello.elf","process_test.elf","ipc_test.elf","signal_test.elf","net_test.elf","echo.elf","cat.elf","ls.elf","pwd.elf","uname.elf","rm.elf","stat.elf","userland_test.elf"};
static const uint8_t *program_starts[RAMFS_PROGRAM_COUNT]={_binary_hello_elf_start,_binary_process_test_elf_start,_binary_ipc_test_elf_start,_binary_signal_test_elf_start,_binary_net_test_elf_start,_binary_echo_elf_start,_binary_cat_elf_start,_binary_ls_elf_start,_binary_pwd_elf_start,_binary_uname_elf_start,_binary_rm_elf_start,_binary_stat_elf_start,_binary_userland_test_elf_start};
static const uint8_t *program_ends[RAMFS_PROGRAM_COUNT]={_binary_hello_elf_end,_binary_process_test_elf_end,_binary_ipc_test_elf_end,_binary_signal_test_elf_end,_binary_net_test_elf_end,_binary_echo_elf_end,_binary_cat_elf_end,_binary_ls_elf_end,_binary_pwd_elf_end,_binary_uname_elf_end,_binary_rm_elf_end,_binary_stat_elf_end,_binary_userland_test_elf_end};
static int streq(const char*a,const char*b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
static struct ramfs_file*find(const char*n){for(uint32_t i=0;i<count;++i)if(streq(files[i].name,n))return &files[i];return 0;}
static void copy_name(char*d,const char*s){uint32_t i=0;while(s[i]&&i<RAMFS_NAME_MAX-1u){d[i]=s[i];++i;}d[i]=0;}
static int program_index(const char*n){for(uint32_t i=0;i<RAMFS_PROGRAM_COUNT;++i)if(streq(n,program_names[i]))return(int)i;return-1;}
void ramfs_init(void){count=0;ramfs_write("readme.txt","Welcome to LionOS.\nBuilt as an experimental 32-bit x86 OS.\n",61u);ramfs_write("version","LionOS 0.7\n",12u);ramfs_write("motd","The LionOS kernel is alive.\n",28u);}
uint32_t ramfs_count(void){return count+RAMFS_PROGRAM_COUNT;}
const char*ramfs_name(uint32_t i){if(i<count)return files[i].name;if(i-count<RAMFS_PROGRAM_COUNT)return program_names[i-count];return 0;}
const char*ramfs_data(const char*n){int p=program_index(n);if(p>=0)return(const char*)program_starts[p];struct ramfs_file*f=find(n);return f?f->data:0;}
uint32_t ramfs_size(const char*n){int p=program_index(n);if(p>=0)return(uint32_t)(program_ends[p]-program_starts[p]);struct ramfs_file*f=find(n);return f?f->size:0;}
int ramfs_exists(const char*n){return program_index(n)>=0||find(n)!=0;}
int ramfs_write(const char*n,const char*d,uint32_t s){if(!n||!*n||!d||program_index(n)>=0)return-1;struct ramfs_file*f=find(n);if(!f){if(count>=RAMFS_MAX_FILES-RAMFS_PROGRAM_COUNT)return-1;f=&files[count++];copy_name(f->name,n);}if(s>RAMFS_DATA_MAX)s=RAMFS_DATA_MAX;for(uint32_t i=0;i<s;++i)f->data[i]=d[i];f->size=s;return 0;}
int ramfs_remove(const char*n){if(program_index(n)>=0)return-1;for(uint32_t i=0;i<count;++i)if(streq(files[i].name,n)){for(uint32_t j=i+1u;j<count;++j)files[j-1u]=files[j];--count;return 0;}return-1;}
