BITS 32

section .multiboot
align 8
multiboot_header:
    dd 0xE85250D6
after_magic:
    dd 0
dd multiboot_header_end - multiboot_header
dd -(0xE85250D6 + 0 + (multiboot_header_end - multiboot_header))
    dw 0
    dw 0
    dd 8
multiboot_header_end:

section .smp_trampoline
align 16
global smp_trampoline_cr3
global smp_trampoline_entry
global smp_trampoline_stack
global smp_trampoline_cpu
BITS 16
smp_trampoline_start:
    cli
    mov dx, 0xE9
    mov al, 'A'
    out dx, al
    xor ax, ax
    mov ds, ax
    lgdt [smp_gdt_ptr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:smp_protected_entry
BITS 32
smp_protected_entry:
    mov dx, 0xE9
    mov al, 'B'
    out dx, al
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov esp, [smp_trampoline_stack]
    mov eax, [smp_trampoline_cr3]
    mov cr3, eax
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    mov dx, 0xE9
    mov al, 'C'
    out dx, al
    mov eax, [smp_trampoline_entry]
    jmp eax
align 8
smp_gdt:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
smp_gdt_end:
smp_gdt_ptr:
    dw smp_gdt_end - smp_gdt - 1
    dd smp_gdt
align 4
smp_trampoline_cr3: dd 0
smp_trampoline_entry: dd 0
smp_trampoline_stack: dd 0
smp_trampoline_cpu: dd 0
smp_trampoline_end:

section .bss
align 16
stack_bottom:
    resb 16384
global stack_top
stack_top:

section .text
global _start
extern kernel_main
_start:
    cli
    mov esp, stack_top
    push ebx
    push eax
    call kernel_main
.hang:
    cli
    hlt
    jmp .hang

global isr_common
extern interrupt_dispatch
isr_common:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    push esp
    call interrupt_dispatch
    add esp, 4
    mov esp, eax
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iretd

%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro
%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR 8
ISR_NOERR 9
ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14
ISR_NOERR 15
ISR_NOERR 16
ISR_NOERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR 30
ISR_NOERR 31
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47
ISR_NOERR 48
ISR_NOERR 128
