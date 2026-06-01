.section .text
.global _start
_start:
    mov $1, %rax
    mov $1, %rdi
    lea message(%rip), %rsi
    mov $message_end - message, %rdx
    syscall

    mov $60, %rax
    xor %rdi, %rdi
    syscall

.section .rodata
message:
    .ascii "klcs-hello: Linux syscall ABI works\n"
message_end:
