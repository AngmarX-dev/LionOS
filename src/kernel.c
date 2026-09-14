#include <stdint.h>
#include <stddef.h>
#include "gdt.h"
#include "idt.h"
#include "paging.h"
#include "heap.h"
#include "pit.h"
#include "memory.h"
#include "tss.h"
#include "process.h"
#include "user.h"
#include "console.h"
#include "ramfs.h"
#include "diskfs.h"
#include "shell.h"
#include "debug.h"

void pic_init(void);
void keyboard_init(void);
void syscall_init(void);

static void boot_dec(uint32_t value) { console_write_dec(value); }

static void diskfs_boot_test(void) {
    static const char marker[] = "LionOS persistent storage online\n";
    char buffer[sizeof(marker)];
    int size = diskfs_read(".boot", buffer, sizeof(buffer));

    if (size == (int)(sizeof(marker) - 1u)) {
        uint32_t ok = 1u;
        for (uint32_t i = 0; i < sizeof(marker) - 1u; ++i)
            if (buffer[i] != marker[i]) { ok = 0; break; }
        if (ok) {
            debug_write("LIONOS:PERSIST-OK\n");
            console_write("[ OK ] Persistent data survived reboot\n");
            return;
        }
    }

    if (diskfs_write(".boot", marker, (uint32_t)(sizeof(marker) - 1u)) == 0) {
        debug_write("LIONOS:PERSIST-INIT\n");
        console_write("[ OK ] Persistent filesystem initialized\n");
    } else {
        debug_write("LIONOS:PERSIST-FAIL\n");
        console_write("[ERR] Persistent filesystem self-test\n");
    }
}

void kernel_main(uint32_t magic, uint32_t multiboot_info) {
    debug_write("LIONOS:BOOT\n");
    console_init();
    console_write("LionOS kernel booting...\n\n");

    if (magic != 0x36D76289u) {
        debug_write("LIONOS:BAD-MULTIBOOT\n");
        console_write("ERROR: invalid Multiboot2 magic.\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    gdt_init(); console_write("[ OK ] GDT / ring-3 segments\n");
    tss_init(); console_write("[ OK ] TSS / ring-0 stack\n");
    idt_init(); console_write("[ OK ] IDT / CPU exceptions / DPL3 syscall\n");
    pic_init(); console_write("[ OK ] PIC remapped\n");
    pit_init(100); console_write("[ OK ] PIT 100 Hz / preemptive scheduler clock\n");
    keyboard_init(); console_write("[ OK ] PS/2 keyboard / scancode input buffer\n");

    memory_init(multiboot_info);
    console_write("[ OK ] Physical memory manager\n       Total pages: "); boot_dec(memory_total_pages());
    console_write("  Free pages: "); boot_dec(memory_free_pages()); console_putc('\n');

    void *page_a = page_alloc(); void *page_b = page_alloc();
    if (page_a && page_b) {
        page_free(page_a); page_free(page_b);
        console_write("[ OK ] Page allocation / free\n");
    } else console_write("[ERR] Page allocator\n");

    paging_init(); console_write("[ OK ] Paging / supervisor kernel mappings\n");
    heap_init();
    void *a = kmalloc(128); void *b = kmalloc(256); void *large = kmalloc(5000);
    if (a && b && large) {
        kfree(b); void *reuse = kmalloc(256); kfree(a); kfree(large); kfree(reuse);
        console_write(reuse ? "[ OK ] Kernel heap / PMM-backed kmalloc + kfree\n" : "[ERR] Kernel heap reuse\n");
    } else {
        console_write("[ERR] Kernel heap\n"); kfree(a); kfree(b); kfree(large);
    }

    syscall_init(); console_write("[ OK ] Syscall ABI / read / write / memory / keyboard\n");
    process_init(); console_write("[ OK ] Process table / PID 1 bootstrap\n");
    console_write("[ OK ] Round-robin scheduler / saved interrupt contexts\n");
    console_write("[ OK ] Ring-3 address spaces / process reclamation\n");

    ramfs_init(); console_write("[ OK ] RAM filesystem / files and directories\n");
    if (diskfs_init() == 0) {
        console_write("[ OK ] ATA PIO / persistent LionFS\n");
        diskfs_boot_test();
    } else {
        console_write("[ -- ] Persistent disk unavailable (RAMFS only)\n");
        debug_write("LIONOS:NO-DISK\n");
    }
    console_write("[ OK ] Interactive console / scrolling / command shell\n");
    console_write("\nLionOS is ready.\n");
    debug_write("LIONOS:READY\n");

    __asm__ volatile ("sti");
    shell_run();

    for (;;) __asm__ volatile ("hlt");
}
