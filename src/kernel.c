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

static void kputc(char c) {
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

static void kputs(const char *s) {
    while (*s) kputc(*s++);
}

void kernel_main(uint32_t magic, uint32_t multiboot_info) {
    (void)multiboot_info;
    clear_screen();

    kputs("LionOS kernel booting...\n\n");
    if (magic != 0x36D76289) {
        kputs("ERROR: invalid Multiboot2 magic.\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    gdt_init();
    kputs("[ OK ] GDT\n");

    idt_init();
    kputs("[ OK ] IDT / CPU exceptions\n");

    pic_init();
    kputs("[ OK ] PIC remapped\n");

    pit_init(100);
    kputs("[ OK ] PIT 100 Hz\n");

    keyboard_init();
    kputs("[ OK ] PS/2 keyboard IRQ1\n");

    paging_init();
    kputs("[ OK ] Paging (first 4 MiB identity mapped)\n");

    heap_init();
    void *a = kmalloc(128);
    void *b = kmalloc(256);
    kputs((a && b) ? "[ OK ] Kernel heap / kmalloc\n" : "[ERR] Kernel heap\n");

    syscall_init();
    kputs("[ OK ] Syscall ABI (INT 0x80)\n\n");
    kputs("LionOS is alive. Interrupts enabled.\n");

    __asm__ volatile ("sti");
    for (;;) __asm__ volatile ("hlt");
}
