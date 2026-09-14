#include <stdint.h>
#include <stddef.h>
#include "gdt.h"
#include "idt.h"
#include "paging.h"
#include "heap.h"
#include "pit.h"

void pic_init(void);
void keyboard_init(void);
void syscall_init(void);

static volatile uint16_t *const VGA = (uint16_t *)0xB8000;
static size_t row;
static size_t col;
static uint8_t color = 0x0F;

static void clear_screen(void) {
    for (size_t i = 0; i < 80 * 25; ++i)
        VGA[i] = ((uint16_t)color << 8) | ' ';
    row = 0;
    col = 0;
}

static void putc(char c) {
    if (c == '\n') {
        col = 0;
        ++row;
    } else {
        VGA[row * 80 + col] = ((uint16_t)color << 8) | (uint8_t)c;
        if (++col >= 80) {
            col = 0;
            ++row;
        }
    }
    if (row >= 25) row = 0;
}

static void puts(const char *s) {
    while (*s) putc(*s++);
}

void kernel_main(uint32_t magic, uint32_t multiboot_info) {
    (void)multiboot_info;
    clear_screen();

    puts("LionOS kernel booting...\n\n");
    if (magic != 0x36D76289) {
        puts("ERROR: invalid Multiboot2 magic.\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    gdt_init();
    puts("[ OK ] GDT\n");

    idt_init();
    puts("[ OK ] IDT / CPU exceptions\n");

    pic_init();
    puts("[ OK ] PIC remapped\n");

    pit_init(100);
    puts("[ OK ] PIT 100 Hz\n");

    keyboard_init();
    puts("[ OK ] PS/2 keyboard IRQ1\n");

    paging_init();
    puts("[ OK ] Paging (first 4 MiB identity mapped)\n");

    heap_init();
    void *a = kmalloc(128);
    void *b = kmalloc(256);
    puts((a && b) ? "[ OK ] Kernel heap / kmalloc\n" : "[ERR] Kernel heap\n");

    syscall_init();
    puts("[ OK ] Syscall ABI (INT 0x80)\n\n");
    puts("LionOS is alive. Interrupts enabled.\n");

    __asm__ volatile ("sti");
    for (;;) __asm__ volatile ("hlt");
}
