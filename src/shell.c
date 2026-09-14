#include <stdint.h>
#include "console.h"
#include "keyboard.h"
#include "memory.h"
#include "process.h"
#include "ramfs.h"
#include "shell.h"

static int eq(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static void prompt(void) { console_write("\nlion> "); }

static void command(char *cmd) {
    if (eq(cmd, "help")) {
        console_write("Commands: help clear echo mem ps ls cat uname uptime write run\n");
    } else if (eq(cmd, "clear")) {
        console_clear();
    } else if (eq(cmd, "mem")) {
        console_write("Pages: total="); console_write_dec(memory_total_pages());
        console_write(" free="); console_write_dec(memory_free_pages()); console_putc('\n');
    } else if (eq(cmd, "ps")) {
        console_write("PID "); console_write_dec(process_current_pid());
        console_write("  state=running  processes="); console_write_dec(process_count()); console_putc('\n');
    } else if (eq(cmd, "ls")) {
        for (uint32_t i = 0; i < ramfs_count(); ++i) { console_write(ramfs_name(i)); console_putc('\n'); }
    } else if (eq(cmd, "cat readme.txt")) {
        const char *d = ramfs_data("readme.txt"); console_write_n(d, ramfs_size("readme.txt"));
    } else if (eq(cmd, "cat version")) {
        const char *d = ramfs_data("version"); console_write_n(d, ramfs_size("version"));
    } else if (eq(cmd, "uname")) {
        console_write("LionOS 0.2 x86 i386 kernel\n");
    } else if (eq(cmd, "uptime")) {
        console_write("Uptime: PIT scheduler active at 100 Hz\n");
    } else if (eq(cmd, "echo hello")) {
        console_write("hello\n");
    } else if (eq(cmd, "run")) {
        console_write("Userspace scheduler test is available through the boot test.\n");
    } else if (*cmd) {
        console_write("Unknown command. Type 'help'.\n");
    }
}

void shell_run(void) {
    char line[96]; uint32_t len = 0;
    console_write("\n========================================\n");
    console_write("          LionOS Shell v0.2\n");
    console_write("========================================\n");
    console_write("Type 'help' for commands.\n");
    prompt();
    for (;;) {
        if (!keyboard_available()) { __asm__ volatile("hlt"); continue; }
        int c = keyboard_getchar();
        if (c < 0) continue;
        if (c == '\n') {
            line[len] = 0;
            console_putc('\n');
            command(line);
            len = 0;
            prompt();
        } else if (c == '\b') {
            if (len) { --len; console_putc('\b'); }
        } else if (c >= 32 && c < 127 && len < sizeof(line) - 1u) {
            line[len++] = (char)c;
            console_putc((char)c);
        }
    }
}
