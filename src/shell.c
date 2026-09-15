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

static void ui_normal(void){console_set_color(CONSOLE_COLOR_LIGHT_GRAY);}
static void ui_accent(void){console_set_color(CONSOLE_COLOR_LIGHT_CYAN);}
static void ui_title(void){console_set_color(CONSOLE_COLOR_YELLOW);}
static void ui_ok(void){console_set_color(CONSOLE_COLOR_LIGHT_GREEN);}
static void ui_error(void){console_set_color(CONSOLE_COLOR_LIGHT_RED);}
static void ui_dim(void){console_set_color(CONSOLE_COLOR_DARK_GRAY);}

static void prompt(void){
    console_putc('\n');
    ui_accent(); console_write("lion");
    ui_normal(); console_write(":");
    ui_accent(); console_write("/ > ");
    ui_normal();
}

static void banner(void){
    ui_title();
    console_write("==============================================================\n");
    console_write("                         L I O N O S                          \n");
    console_write("                  32-bit x86 experimental OS                 \n");
    console_write("==============================================================\n");
    ui_dim();
    console_write("SMP: 2 CPU test path   |   VFS: RAMFS + LionFS   |   ELF32\n");
    console_write("Type 'help' for commands.\n");
    ui_normal();
}

static void cmd_ls(void){uint32_t count=vfs_count();if(!count){ui_dim();console_write("(no files)\n");ui_normal();return;}for(uint32_t i=0;i<count;++i){const char *name=vfs_name(i);if(!name)continue;struct vfs_stat st;if(vfs_stat_path(name,&st)<0)continue;ui_accent();console_write(name);ui_normal();console_write("  ");console_write_dec(st.size);console_write(" bytes  ");ui_dim();console_write(st.backend==VFS_BACKEND_DISKFS?"[disk]":"[ram]");ui_normal();console_putc('\n');}}

static void cmd_cat(char *arg){arg=skip_spaces(arg);if(!*arg){ui_error();console_write("usage: cat <file>\n");ui_normal();return;}int fd=vfs_open(arg,LIONOS_O_READ);if(fd<0){ui_error();console_write("cat: file not found\n");ui_normal();return;}int n=vfs_read(fd,io_buffer,SHELL_IO_MAX);vfs_close(fd);if(n<0){ui_error();console_write("cat: read failed\n");ui_normal();return;}console_write_n((const char*)io_buffer,(uint32_t)n);if(n&&io_buffer[n-1]!='\n')console_putc('\n');}

static void cmd_write(char *arg){arg=skip_spaces(arg);if(!*arg){ui_error();console_write("usage: write <file> <text>\n");ui_normal();return;}char *name=arg;while(*arg&&*arg!=' ')++arg;if(!*arg){ui_error();console_write("usage: write <file> <text>\n");ui_normal();return;}*arg++=0;arg=skip_spaces(arg);if(!*arg){ui_error();console_write("usage: write <file> <text>\n");ui_normal();return;}uint32_t n=text_len(arg);if(n>SHELL_IO_MAX)n=SHELL_IO_MAX;int fd=vfs_open(name,LIONOS_O_WRITE);if(fd<0){ui_error();console_write("write: open failed\n");ui_normal();return;}int r=vfs_write(fd,arg,n);vfs_close(fd);if(r<0){ui_error();console_write("write: failed\n");ui_normal();}else{ui_ok();console_write("write: ok\n");ui_normal();}}

static void cmd_touch(char *arg){arg=skip_spaces(arg);if(!*arg){ui_error();console_write("usage: touch <file>\n");ui_normal();return;}int fd=vfs_open(arg,LIONOS_O_WRITE);if(fd<0){ui_error();console_write("touch: failed\n");ui_normal();return;}vfs_close(fd);ui_ok();console_write("touch: ok\n");ui_normal();}

static void cmd_rm(char *arg){arg=skip_spaces(arg);if(!*arg){ui_error();console_write("usage: rm <file>\n");ui_normal();return;}if(vfs_remove(arg)<0){ui_error();console_write("rm: file not found\n");ui_normal();}else{ui_ok();console_write("rm: ok\n");ui_normal();}}

static void cmd_stat(char *arg){arg=skip_spaces(arg);if(!*arg){ui_error();console_write("usage: stat <file>\n");ui_normal();return;}struct vfs_stat st;if(vfs_stat_path(arg,&st)<0){ui_error();console_write("stat: file not found\n");ui_normal();return;}console_write("name: ");ui_accent();console_write(arg);ui_normal();console_write("\nsize: ");console_write_dec(st.size);console_write(" bytes\nbackend: ");ui_dim();console_write(st.backend==VFS_BACKEND_DISKFS?"persistent LionFS":"RAMFS");ui_normal();console_putc('\n');}

static void cmd_ps(void){console_write("PID   STATE      ENTRY\n");for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i){struct process *p=process_at(i);if(!p||p->state==PROCESS_UNUSED)continue;console_write_dec(p->pid);console_write("    ");ui_accent();console_write(process_state_name(p->state));ui_normal();console_write("    ");console_write_hex(p->entry);console_putc('\n');}}

static void command(char *cmd){
    if(eq(cmd,"help")){
        ui_title(); console_write("LionOS Shell v0.8\n"); ui_normal();
        console_write("System : "); ui_accent(); console_write("help clear mem ps uname uptime version about\n"); ui_normal();
        console_write("Files  : "); ui_accent(); console_write("pwd ls cat write touch rm stat\n"); ui_normal();
        console_write("Exec   : "); ui_accent(); console_write("run <program.elf>\n"); ui_normal();
        ui_dim(); console_write("RAMFS and persistent LionFS are exposed through the VFS.\n"); ui_normal();
    }
    else if(eq(cmd,"clear")){console_clear();banner();}
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
    else if(prefix(cmd,"run ")){char *n=skip_spaces(cmd+4);if(!*n){ui_error();console_write("usage: run <program.elf>\n");ui_normal();return;}int pid=exec_run_file(n);if(pid<0){ui_error();console_write("run: invalid or unsupported ELF32 file\n");ui_normal();}else{ui_ok();console_write("run: started PID ");console_write_dec((uint32_t)pid);console_putc('\n');ui_normal();}}
    else if(eq(cmd,"uname"))console_write("LionOS 0.8 x86 i386 kernel\n");
    else if(eq(cmd,"version"))console_write("LionOS version 0.8\n");
    else if(eq(cmd,"about")){ui_title();console_write("LionOS\n");ui_normal();console_write("Experimental 32-bit x86 operating system.\nKernel, paging, ring-3 processes, scheduler, syscalls, RAMFS, LionFS, VFS, ELF32, LAPIC and SMP bring-up.\n");}
    else if(eq(cmd,"uptime")){ui_ok();console_write("Uptime: scheduler running at 100 Hz.\n");ui_normal();}
    else if(*cmd){ui_error();console_write("Unknown command. Type 'help'.\n");ui_normal();}
}

void shell_run(void){
    char line[SHELL_LINE_MAX];
    uint32_t len=0;
    console_clear();
    banner();
    prompt();
    for(;;){
        if(!keyboard_available()){__asm__ volatile("hlt");continue;}
        int c=keyboard_getchar();
        if(c<0)continue;
        if(c=='\n'){line[len]=0;console_putc('\n');command(line);len=0;prompt();}
        else if(c=='\b'){if(len){--len;console_putc('\b');}}
        else if(c>=32&&c<127&&len<SHELL_LINE_MAX-1u){line[len++]=(char)c;console_putc((char)c);}
    }
}
