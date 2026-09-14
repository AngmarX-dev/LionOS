#include <stdint.h>
#include "console.h"
#include "diskfs.h"
#include "exec.h"
#include "keyboard.h"
#include "memory.h"
#include "process.h"
#include "ramfs.h"
#include "shell.h"

#define SHELL_LINE_MAX 96u
#define DISK_CAT_BUFFER 4096u

static uint8_t disk_cat_buffer[DISK_CAT_BUFFER];

static int eq(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static int prefix(const char *s, const char *p) { while (*p) { if (*s++ != *p++) return 0; } return 1; }
static char *skip_spaces(char *s) { while (*s == ' ') ++s; return s; }
static void prompt(void) { console_write("\nlion> "); }

static void cmd_cat(char *arg) {
    arg = skip_spaces(arg); if (!*arg) { console_write("usage: cat <file>\n"); return; }
    const char *data = ramfs_data(arg); if (!data) { console_write("cat: file not found\n"); return; }
    uint32_t size = ramfs_size(arg); console_write_n(data, size);
    if (size && data[size - 1u] != '\n') console_putc('\n');
}

static void cmd_write(char *arg) {
    arg = skip_spaces(arg); if (!*arg) { console_write("usage: write <file> <text>\n"); return; }
    char *name = arg; while (*arg && *arg != ' ') ++arg;
    if (!*arg) { console_write("usage: write <file> <text>\n"); return; }
    *arg++ = 0; arg = skip_spaces(arg); if (!*arg) { console_write("usage: write <file> <text>\n"); return; }
    uint32_t n = 0; while (arg[n] && n < 256u) ++n;
    if (ramfs_write(name, arg, n) < 0) console_write("write: failed\n"); else console_write("write: ok\n");
}

static void cmd_disk_cat(char *arg) {
    arg = skip_spaces(arg); if (!*arg) { console_write("usage: dcat <file>\n"); return; }
    if (!diskfs_available()) { console_write("dcat: no persistent disk\n"); return; }
    int size = diskfs_read(arg, disk_cat_buffer, sizeof(disk_cat_buffer));
    if (size < 0) { console_write("dcat: file not found or read failed\n"); return; }
    console_write_n((const char *)disk_cat_buffer, (uint32_t)size);
    if (size && disk_cat_buffer[size - 1] != '\n') console_putc('\n');
}

static void cmd_disk_write(char *arg) {
    arg = skip_spaces(arg); if (!*arg) { console_write("usage: dwrite <file> <text>\n"); return; }
    if (!diskfs_available()) { console_write("dwrite: no persistent disk\n"); return; }
    char *name = arg; while (*arg && *arg != ' ') ++arg;
    if (!*arg) { console_write("usage: dwrite <file> <text>\n"); return; }
    *arg++ = 0; arg = skip_spaces(arg); if (!*arg) { console_write("usage: dwrite <file> <text>\n"); return; }
    uint32_t n = 0; while (arg[n] && n < DISKFS_MAX_FILE_SIZE) ++n;
    if (diskfs_write(name, arg, n) < 0) console_write("dwrite: failed\n"); else console_write("dwrite: ok (persistent)\n");
}

static void cmd_disk_ls(void) {
    if (!diskfs_available()) { console_write("dls: no persistent disk\n"); return; }
    for (uint32_t i = 0; i < DISKFS_MAX_FILES; ++i) {
        const char *name = diskfs_name(i);
        if (!name) continue;
        console_write(name); console_write("  "); console_write_dec(diskfs_size(name)); console_write(" bytes\n");
    }
}

static void cmd_ps(void) {
    console_write("PID   STATE      ENTRY\n");
    for (uint32_t i = 0; i < LIONOS_PROCESS_MAX; ++i) {
        struct process *p = process_at(i); if (!p || p->state == PROCESS_UNUSED) continue;
        console_write_dec(p->pid); console_write("    "); console_write(process_state_name(p->state));
        console_write("    "); console_write_hex(p->entry); console_putc('\n');
    }
}

static void command(char *cmd) {
    if (eq(cmd, "help")) {
        console_write("LionOS Shell v0.6\n");
        console_write("help clear echo mem ps ls cat write touch rm run dls dcat dwrite drm uname uptime version about\n");
        console_write("run <program.elf> launches a validated ELF32 user process\n");
        console_write("dls/dcat/dwrite/drm access persistent ATA-backed LionFS files\n");
    } else if (eq(cmd, "clear")) console_clear();
    else if (prefix(cmd, "echo ")) { console_write(cmd + 5); console_putc('\n'); }
    else if (eq(cmd, "mem")) { console_write("Pages: total="); console_write_dec(memory_total_pages()); console_write(" free="); console_write_dec(memory_free_pages()); console_putc('\n'); }
    else if (eq(cmd, "ps")) cmd_ps();
    else if (eq(cmd, "ls")) { for (uint32_t i = 0; i < ramfs_count(); ++i) { console_write(ramfs_name(i)); console_putc('\n'); } }
    else if (prefix(cmd, "cat ")) cmd_cat(cmd + 4);
    else if (prefix(cmd, "write ")) cmd_write(cmd + 6);
    else if (prefix(cmd, "touch ")) { char *n = skip_spaces(cmd + 6); if (!*n || ramfs_write(n, "", 0) < 0) console_write("touch: failed\n"); else console_write("touch: ok\n"); }
    else if (prefix(cmd, "rm ")) { char *n = skip_spaces(cmd + 3); if (!*n || ramfs_remove(n) < 0) console_write("rm: file not found\n"); else console_write("rm: ok\n"); }
    else if (eq(cmd, "dls")) cmd_disk_ls();
    else if (prefix(cmd, "dcat ")) cmd_disk_cat(cmd + 5);
    else if (prefix(cmd, "dwrite ")) cmd_disk_write(cmd + 7);
    else if (prefix(cmd, "drm ")) { char *n = skip_spaces(cmd + 4); if (!*n || diskfs_remove(n) < 0) console_write("drm: file not found or remove failed\n"); else console_write("drm: ok\n"); }
    else if (prefix(cmd, "run ")) {
        char *n = skip_spaces(cmd + 4); if (!*n) { console_write("usage: run <program.elf>\n"); return; }
        int pid = exec_run_file(n);
        if (pid < 0) console_write("run: invalid or unsupported ELF32 file\n");
        else { console_write("run: started PID "); console_write_dec((uint32_t)pid); console_putc('\n'); }
    }
    else if (eq(cmd, "uname")) console_write("LionOS 0.6 x86 i386 kernel\n");
    else if (eq(cmd, "version")) console_write("LionOS version 0.6\n");
    else if (eq(cmd, "about")) console_write("LionOS: experimental 32-bit x86 operating system.\nKernel, paging, ring-3 processes, scheduler, syscalls, RAMFS, LionFS and ELF32 loading.\n");
    else if (eq(cmd, "uptime")) console_write("Uptime: scheduler running at 100 Hz.\n");
    else if (*cmd) console_write("Unknown command. Type 'help'.\n");
}

void shell_run(void) {
    char line[SHELL_LINE_MAX]; uint32_t len = 0;
    console_write("\n========================================\nLionOS Shell v0.6\n========================================\n");
    console_write("RAMFS + persistent LionFS + ELF32 launcher enabled.\nType 'help' for commands.\n"); prompt();
    for (;;) {
        if (!keyboard_available()) { __asm__ volatile("hlt"); continue; }
        int c = keyboard_getchar(); if (c < 0) continue;
        if (c == '\n') { line[len] = 0; console_putc('\n'); command(line); len = 0; prompt(); }
        else if (c == '\b') { if (len) { --len; console_putc('\b'); } }
        else if (c >= 32 && c < 127 && len < SHELL_LINE_MAX - 1u) { line[len++] = (char)c; console_putc((char)c); }
    }
}
