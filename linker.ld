ENTRY(_start)

SECTIONS
{
    /* SIPI vectors address the first 1 MiB in 4 KiB units. */
    . = 0x8000;
    .smp_trampoline : ALIGN(16)
    {
        KEEP(*(.smp_trampoline))
        KEEP(*(.smp_trampoline.*))
    }

    . = 1M;
    _kernel_start = .;

    .text : ALIGN(4K)
    {
        KEEP(*(.multiboot))
        *(.text*)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata*)
    }

    .data : ALIGN(4K)
    {
        *(.data*)
    }

    .bss : ALIGN(4K)
    {
        *(COMMON)
        *(.bss*)
    }

    . = ALIGN(4K);
    _kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note.GNU-stack)
    }
}
