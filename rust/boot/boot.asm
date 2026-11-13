BITS 32

section .multiboot
align 8
mb2_header:
    dd 0xE85250D6
    dd 0
    dd mb2_header_end - mb2_header
    dd -(0xE85250D6 + 0 + (mb2_header_end - mb2_header))

    ; Request a firmware-selected framebuffer. Width/height/depth remain
    ; unspecified so GRUB keeps control of the actual display mode.
    dw 5
    dw 0
    dd 20
    dd 0
    dd 0
    dd 0

    dw 0
    dw 0
    dd 8
mb2_header_end:

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

section .text
global _start
extern rust_kernel_main

_start:
    cli
    mov esp, stack_top
    xor ebp, ebp
    cld

    ; Keep the i386 SysV call-site stack 16-byte aligned.
    sub esp, 8

    ; Multiboot2: EAX = boot magic, EBX = information structure.
    push ebx
    push eax
    call rust_kernel_main

.halt:
    cli
    hlt
    jmp .halt

section .note.GNU-stack noalloc noexec nowrite progbits
