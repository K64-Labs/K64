.section .text
.global _start
.type _start, @function

_start:
    mov $1, %rax
    lea msg(%rip), %rdi
    mov $msg_len, %rsi
    int $0x80

    mov $0, %rax
    mov $42, %rdi
    int $0x80

1:
    hlt
    jmp 1b

.section .rodata
msg:
    .ascii "hello from K64 user mode\n"
.set msg_len, . - msg
