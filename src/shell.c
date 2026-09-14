#include <stdint.h>
#include "console.h"
#include "exec.h"
#include "keyboard.h"
#include "memory.h"
#include "process.h"
#include "shell.h"
#include "vfs.h"
#include "vfs_uapi.h"

#define SHELL_LINE_MAX 128u
#define SHELL_IO_MAX 4096u

static uint8_t io_buffer[SHELL_IO_MAX];

static int eq(const char *a,const char *b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
static int prefix(const char *s,const char *p){while(*p){if(*s++!=*p++)return 0;}return 1;}
static char *skip_spaces(char *s){while(*s==' ')++s;return s;}
static uint32_t text_len(const char *s){uint32_t n=0;while(s[n])++n;return n;}
static void prompt(void){console_write("\nlion:/ > ");}

static void cmd_ls(void){uint32_t count=vfs_count();if(!count){console_write("(no files)\n");return;}for(uint32_t i=0;i<count;++i){const char *name=vfs_name(i);if(!name)continue;struct vfs_stat st;if(vfs_stat_path(name,&st)<0)continue;console_write(name);console_write("  ");console_write_dec(st.size);console_write(" bytes  ");console_write(st.backend==VFS_BACKEND_DISKFS?"[disk]":"[ram]");console_putc('\n');}}

static void cmd_cat(char *arg){arg=skip_spaces(arg);if(!*arg){console_write("usage: cat <file>\n");return;}int fd=vfs_open(arg,LIONOS_O_READ);if(fd<0){console_write("cat: file not found\n");return;}int n=vfs_read(fd,io_buffer,SHELL_IO_MAX);vfs_close(fd);if(n<0){console_write("cat: read failed\n");return;}console_write_n((const char*)io_buffer,(uint32_t)n);if(n&&io_buffer[n-1]!='\n')console_putc('\n');}

static void cmd_write(char *arg){arg=skip_spaces(arg);if(!*arg){console_write("usage: write <file> <text>\n");return;}char *name=arg;while(*arg&&*arg!=' ')++arg;if(!*arg){console_write("usage: write <file> <text>\n");return;}*arg++=0;arg=skip_spaces(arg);if(!*arg){console_write("usage: write <file> <text>\n");return;}uint32_t n=text_len(arg);if(n>SHELL_IO_MAX)n=SHELL_IO_MAX;int fd=vfs_open(name,LIONOS_O_WRITE);if(fd<0){console_write("write: open failed\n");return;}int r=vfs_write(fd,arg,n);vfs_close(fd);if(r<0)console_write("write: failed\n");else console_write("write: ok\n");}

static void cmd_touch(char *arg){arg=skip_spaces(arg);if(!*arg){console_write("usage: touch <file>\n");return;}int fd=vfs_open(arg,LIONOS_O_WRITE);if(fd<0){console_write("touch: failed\n");return;}vfs_close(fd);console_write("touch: ok\n");}

static void cmd_rm(char *arg){arg=skip_spaces(arg);if(!*arg){console_write("usage: rm <file>\n");return;}if(vfs_remove(arg)<0)console_write("rm: file not found\n");else console_write("rm: ok\n");}

static void cmd_stat(char *arg){arg=skip_spaces(arg);if(!*arg){console_write("usage: stat <file>\n");return;}struct vfs_stat st;if(vfs_stat_path(arg,&st)<0){console_write("stat: file not found\n");return;}console_write("name: ");console_write(arg);console_write("\nsize: ");console_write_dec(st.size);console_write(" bytes\nbackend: ");console_write(st.backend==VFS_BACKEND_DISKFS?"persistent LionFS":"RAMFS");console_putc('\n');}

static void cmd_ps(void){console_write("PID   STATE      ENTRY\n");for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i){struct process *p=process_at(i);if(!p||p->state==PROCESS_UNUSED)continue;console_write_dec(p->pid);console_write("    ");console_write(process_state_name(p->state));console_write("    ");console_write_hex(p->entry);console_putc('\n');}}

static void command(char *cmd){
    if(eq(cmd,"help")){console_write("LionOS Shell v0.7\n");console_write("help  clear  echo  pwd  ls  cat  write  touch  rm  stat\n");console_write("mem   ps     run   uname uptime version about\n");console_write("run <program.elf> launches a validated ELF32 user process.\n");console_write("Files are unified across RAMFS and persistent LionFS.\n");}
    else if(eq(cmd,"clear"))console_clear();
    else if(prefix(cmd,"echo ")){console_write(cmd+5);console_putc('\n');}
    else if(eq(cmd,"pwd"))console_write("/\n");
    else if(eq(cmd,"ls"))cmd_ls();
    else if(prefix(cmd,"cat "))cmd_cat(cmd+4);
    else if(prefix(cmd,"write "))cmd_write(cmd+6);
    else if(prefix(cmd,"touch "))cmd_touch(cmd+6);
    else if(prefix(cmd,"rm "))cmd_rm(cmd+3);
    else if(prefix(cmd,"stat "))cmd_stat(cmd+5);
    else if(eq(cmd,"mem")){console_write("Pages: total=");console_write_dec(memory_total_pages());console_write(" free=");console_write_dec(memory_free_pages());console_putc('\n');}
    else if(eq(cmd,"ps"))cmd_ps();
    else if(prefix(cmd,"run ")){char *n=skip_spaces(cmd+4);if(!*n){console_write("usage: run <program.elf>\n");return;}int pid=exec_run_file(n);if(pid<0)console_write("run: invalid or unsupported ELF32 file\n");else{console_write("run: started PID ");console_write_dec((uint32_t)pid);console_putc('\n');}}
    else if(eq(cmd,"uname"))console_write("LionOS 0.7 x86 i386 kernel\n");
    else if(eq(cmd,"version"))console_write("LionOS version 0.7\n");
    else if(eq(cmd,"about"))console_write("LionOS: experimental 32-bit x86 operating system.\nKernel, paging, ring-3 processes, scheduler, syscalls, RAMFS, LionFS, VFS and ELF32 loading.\n");
    else if(eq(cmd,"uptime"))console_write("Uptime: scheduler running at 100 Hz.\n");
    else if(*cmd)console_write("Unknown command. Type 'help'.\n");
}

void shell_run(void){char line[SHELL_LINE_MAX];uint32_t len=0;console_write("\n========================================\nLionOS Shell v0.7\n========================================\n");console_write("Unified RAMFS + persistent LionFS + ELF32 utilities enabled.\nType 'help' for commands.\n");prompt();for(;;){if(!keyboard_available()){__asm__ volatile("hlt");continue;}int c=keyboard_getchar();if(c<0)continue;if(c=='\n'){line[len]=0;console_putc('\n');command(line);len=0;prompt();}else if(c=='\b'){if(len){--len;console_putc('\b');}}else if(c>=32&&c<127&&len<SHELL_LINE_MAX-1u){line[len++]=(char)c;console_putc((char)c);}}}
