.section .text
.global _start
.type _start, @function

_start:
    xor %rdi, %rdi
    xor %rax, %rax
    int $0x80
1:
    hlt
    jmp 1b
