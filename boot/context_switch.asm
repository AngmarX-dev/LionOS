BITS 32
section .text
global lionos_context_switch

; void lionos_context_switch(KernelContext *old, const KernelContext *next)
; cdecl; saves callee-saved registers, EFLAGS and CR3, then restores next.
; Context offsets: esp=0, eip=4, ebp=8, ebx=12, esi=16, edi=20,
;                  eflags=24, cr3=28.
lionos_context_switch:
    push ebp
    push ebx
    push esi
    push edi
    pushfd

    mov eax, [esp + 24]       ; old context argument
    mov edx, [esp + 28]       ; next context argument

    mov [eax + 0], esp
    mov ecx, [esp + 20]
    mov [eax + 4], ecx
    mov ecx, [esp + 16]
    mov [eax + 8], ecx
    mov ecx, [esp + 12]
    mov [eax + 12], ecx
    mov ecx, [esp + 8]
    mov [eax + 16], ecx
    mov ecx, [esp + 4]
    mov [eax + 20], ecx
    mov ecx, [esp + 0]
    mov [eax + 24], ecx

    mov ecx, cr3
    mov [eax + 28], ecx

    ; A zero CR3 means retain the current address space.
    mov ecx, [edx + 28]
    test ecx, ecx
    jz .keep_cr3
    mov cr3, ecx
.keep_cr3:
    mov esp, [edx + 0]
    popfd
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
