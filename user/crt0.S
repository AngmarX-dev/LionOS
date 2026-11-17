.section .text
.global _start
.extern main
.extern lion_exit_code_bridge
.type _start, @function

_start:
    # exec builds: [argc] ... [argv[0], NULL] ... at the initial ESP.
    mov (%esp), %eax
    lea 8(%esp), %edx

    push %edx
    push %eax
    call main
    add $8, %esp

    push %eax
    call lion_exit_code_bridge

1:
    cli
    hlt
    jmp 1b

.size _start, .-_start

.global lion_signal_trampoline
.type lion_signal_trampoline, @function
lion_signal_trampoline:
    mov $64, %eax
    int $0x80
    ud2
.size lion_signal_trampoline, .-lion_signal_trampoline

