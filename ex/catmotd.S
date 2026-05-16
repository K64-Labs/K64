.section .text
.global _start
.type _start, @function

_start:
    sub $128, %rsp

    lea path(%rip), %rdi
    mov $4, %rax
    int $0x80
    mov %rax, %rbx
    cmp $0, %rax
    jl fail

    mov %rbx, %rdi
    mov %rsp, %rsi
    mov $128, %rdx
    mov $5, %rax
    int $0x80
    cmp $0, %rax
    jl fail
    mov %rax, %r12

    lea prefix(%rip), %rdi
    mov $9, %rsi
    mov $1, %rax
    int $0x80

    mov %rsp, %rdi
    mov %r12, %rsi
    mov $1, %rax
    int $0x80

    mov %rbx, %rdi
    mov $6, %rax
    int $0x80

    mov $0, %rax
    xor %rdi, %rdi
    int $0x80

fail:
    mov $0, %rax
    mov $1, %rdi
    int $0x80

.section .rodata
path:
    .asciz "/etc/motd"
prefix:
    .ascii "motd => "
