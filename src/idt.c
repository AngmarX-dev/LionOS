#include <stdint.h>
#include "idt.h"
#include "io.h"
#include "keyboard.h"
#include "process.h"
#include "syscall.h"
#include "tss.h"
#include "console.h"

struct idt_entry { uint16_t base_low; uint16_t selector; uint8_t zero; uint8_t flags; uint16_t base_high; } __attribute__((packed));
struct idt_ptr { uint16_t limit; uint32_t base; } __attribute__((packed));
static struct idt_entry idt[256]; static struct idt_ptr idtp;

#define ISR_DECL(n) extern void isr##n(void)
ISR_DECL(0); ISR_DECL(1); ISR_DECL(2); ISR_DECL(3); ISR_DECL(4); ISR_DECL(5); ISR_DECL(6); ISR_DECL(7);
ISR_DECL(8); ISR_DECL(9); ISR_DECL(10); ISR_DECL(11); ISR_DECL(12); ISR_DECL(13); ISR_DECL(14); ISR_DECL(15);
ISR_DECL(16); ISR_DECL(17); ISR_DECL(18); ISR_DECL(19); ISR_DECL(20); ISR_DECL(21); ISR_DECL(22); ISR_DECL(23);
ISR_DECL(24); ISR_DECL(25); ISR_DECL(26); ISR_DECL(27); ISR_DECL(28); ISR_DECL(29); ISR_DECL(30); ISR_DECL(31);
ISR_DECL(32); ISR_DECL(33); ISR_DECL(34); ISR_DECL(35); ISR_DECL(36); ISR_DECL(37); ISR_DECL(38);
ISR_DECL(39); ISR_DECL(40); ISR_DECL(41); ISR_DECL(42); ISR_DECL(43); ISR_DECL(44); ISR_DECL(45); ISR_DECL(46); ISR_DECL(47);

static void idt_set_gate(uint8_t n, uint32_t base, uint8_t flags) {
    idt[n].base_low = (uint16_t)(base & 0xFFFFu); idt[n].selector = 0x08; idt[n].zero = 0;
    idt[n].flags = flags; idt[n].base_high = (uint16_t)((base >> 16) & 0xFFFFu);
}
void idt_init(void) {
    for (int i = 0; i < 256; ++i) idt[i] = (struct idt_entry){0};
    void (*handlers[48])(void) = { isr0,isr1,isr2,isr3,isr4,isr5,isr6,isr7,isr8,isr9,isr10,isr11,isr12,isr13,isr14,isr15,isr16,isr17,isr18,isr19,isr20,isr21,isr22,isr23,isr24,isr25,isr26,isr27,isr28,isr29,isr30,isr31,isr32,isr33,isr34,isr35,isr36,isr37,isr38,isr39,isr40,isr41,isr42,isr43,isr44,isr45,isr46,isr47 };
    for (int i = 0; i < 48; ++i) idt_set_gate((uint8_t)i, (uint32_t)handlers[i], 0x8E);
    extern void isr128(void); idt_set_gate(128, (uint32_t)isr128, 0xEE);
    idtp.limit = sizeof(idt) - 1; idtp.base = (uint32_t)&idt; __asm__ volatile ("lidt %0" : : "m"(idtp));
}
static volatile uint32_t ticks;
static void page_fault_dump(uint32_t *frame, uint32_t fault_address) {
    console_write("\n[PAGE FAULT] user process terminated\nPID: "); console_write_dec(process_current_pid());
    console_write(" address: 0x"); console_write_hex(fault_address); console_write(" error: 0x"); console_write_hex(frame[13]); console_write("\n");
}
uint32_t *interrupt_dispatch(uint32_t *frame) {
    uint32_t vector = frame[12];
    if (vector == 128) {
        uint32_t number = frame[11];
        if (number == SYS_FORK) {
            frame[11] = process_fork_current(frame);
            return frame;
        }
        frame[11] = syscall_handle(number, frame[8], frame[10], frame[9]);
        if (number == SYS_YIELD || number == SYS_EXIT || number == SYS_EXEC ||
            (number == SYS_WAITPID && frame[11] == (uint32_t)-2)) {
            uint32_t *next = process_schedule(frame); struct process *cur = process_current();
            if (cur) tss_set_kernel_stack(process_kernel_stack_top(cur)); return next ? next : frame;
        }
        return frame;
    }
    if (vector == 32) {
        ++ticks; outb(0x20, 0x20); uint32_t *next = process_schedule(frame); struct process *cur = process_current();
        if (cur) tss_set_kernel_stack(process_kernel_stack_top(cur)); return next ? next : frame;
    }
    if (vector == 33) { keyboard_handle_scancode(inb(0x60)); outb(0x20, 0x20); }
    else if (vector >= 32 && vector < 48) { if (vector >= 40) outb(0xA0, 0x20); outb(0x20, 0x20); }
    else if (vector == 14) {
        uint32_t fault_address; __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_address));
        if ((frame[15] & 0x3u) == 0x3u) {
            page_fault_dump(frame, fault_address); process_exit_current(139u); uint32_t *next = process_schedule(frame); struct process *cur = process_current();
            if (cur) tss_set_kernel_stack(process_kernel_stack_top(cur)); return next ? next : frame;
        }
        console_write("\n[FATAL] kernel page fault at 0x"); console_write_hex(fault_address); console_write(" error=0x"); console_write_hex(frame[13]); console_write("\nSystem halted.\n");
        for (;;) __asm__ volatile ("cli; hlt");
    } else if (vector < 32) __asm__ volatile ("cli");
    return frame;
}
