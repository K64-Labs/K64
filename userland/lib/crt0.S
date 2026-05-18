.section .text
.global _start
.type _start, @function

_start:
    mov (%rsp), %rdi
    lea 8(%rsp), %rsi
    call main
    mov %rax, %rdi
    call k64_exit

1:
    hlt
    jmp 1b
