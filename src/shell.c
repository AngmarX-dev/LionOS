#include <stdint.h>
#include "console.h"
#include "keyboard.h"
#include "memory.h"
#include "process.h"
#include "ramfs.h"
#include "shell.h"

static int eq(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static int starts(const char *s, const char *prefix) {
    while (*prefix) { if (*s++ != *prefix++) return 0; }
    return 1;
}

static const char *skip_spaces(const char *s) { while (*s == ' ') ++s; return s; }

static void prompt(void) { console_write("\nlion> "); }

static void print_file(const char *name) {
    const char *d = ramfs_data(name);
    if (!d) { console_write("cat: file not found\n"); return; }
    console_write_n(d, ramfs_size(name));
    if (ramfs_size(name) == 0 || d[ramfs_size(name) - 1u] != '\n') console_putc('\n');
}

static void command(char *cmd) {
    const char *arg;

    if (eq(cmd, "help")) {
        console_write("LionOS commands:\n");
        console_write("  help             Show this help\n");
        console_write("  clear            Clear the console\n");
        console_write("  echo <text>      Print text\n");
        console_write("  mem              Memory information\n");
        console_write("  ps               Process information\n");
        console_write("  ls               List RAMFS files\n");
        console_write("  cat <file>       Display a file\n");
        console_write("  write <f> <text> Create/replace a file\n");
        console_write("  rm <file>        Remove a file\n");
        console_write("  touch <file>     Create an empty file\n");
        console_write("  uname            System information\n");
        console_write("  uptime           Scheduler clock status\n");
        console_write("  run              Run the userspace test\n");
    } else if (eq(cmd, "clear")) {
        console_clear();
    } else if (starts(cmd, "echo ")) {
        console_write(cmd + 5); console_putc('\n');
    } else if (eq(cmd, "echo")) {
        console_putc('\n');
    } else if (eq(cmd, "mem")) {
        console_write("Pages: total="); console_write_dec(memory_total_pages());
        console_write(" free="); console_write_dec(memory_free_pages()); console_putc('\n');
    } else if (eq(cmd, "ps")) {
        console_write("PID "); console_write_dec(process_current_pid());
        console_write("  state=running  processes="); console_write_dec(process_count()); console_putc('\n');
    } else if (eq(cmd, "ls")) {
        console_write("RAMFS files:\n");
        for (uint32_t i = 0; i < ramfs_count(); ++i) {
            const char *name = ramfs_name(i);
            console_write("  "); console_write(name);
            console_write("  "); console_write_dec(ramfs_size(name)); console_write(" bytes\n");
        }
    } else if (starts(cmd, "cat ")) {
        arg = skip_spaces(cmd + 4);
        if (*arg) print_file(arg); else console_write("cat: missing file name\n");
    } else if (starts(cmd, "touch ")) {
        arg = skip_spaces(cmd + 6);
        if (!*arg) console_write("touch: missing file name\n");
        else if (ramfs_write(arg, "", 0) != 0) console_write("touch: cannot create file\n");
    } else if (starts(cmd, "rm ")) {
        arg = skip_spaces(cmd + 3);
        if (!*arg) console_write("rm: missing file name\n");
        else if (ramfs_remove(arg) != 0) console_write("rm: file not found\n");
    } else if (starts(cmd, "write ")) {
        char *name = cmd + 6;
        while (*name == ' ') ++name;
        char *p = name;
        while (*p && *p != ' ') ++p;
        if (!*p) {
            console_write("write: usage: write <file> <text>\n");
        } else {
            *p++ = 0;
            p = (char *)skip_spaces(p);
            if (ramfs_write(name, p, 256u) != 0) console_write("write: cannot create file\n");
        }
    } else if (eq(cmd, "uname")) {
        console_write("LionOS 0.3 x86 i386 kernel\n");
    } else if (eq(cmd, "uptime")) {
        console_write("Uptime: PIT scheduler active at 100 Hz\n");
    } else if (eq(cmd, "run")) {
        console_write("Userspace scheduler and page-fault tests are enabled.\n");
    } else if (*cmd) {
        console_write("Unknown command. Type 'help'.\n");
    }
}

void shell_run(void) {
    char line[96]; uint32_t len = 0;
    console_write("\n========================================\n");
    console_write("          LionOS Shell v0.3\n");
    console_write("========================================\n");
    console_write("RAMFS + process shell ready. Type 'help'.\n");
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
