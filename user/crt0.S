.section .text
.global _start
.extern main
.extern lion_exit_code
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
    call lion_exit_code

1:
    hlt
    jmp 1b

.size _start, .-_start
