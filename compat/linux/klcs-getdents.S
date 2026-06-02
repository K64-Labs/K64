.section .text
.global _start
_start:
    mov $257, %rax
    mov $-100, %rdi
    lea root_path(%rip), %rsi
    mov $0x10000, %rdx
    xor %r10, %r10
    syscall
    test %rax, %rax
    js fail
    mov %rax, %r12

    mov $217, %rax
    mov %r12, %rdi
    lea dent_buffer(%rip), %rsi
    mov $512, %rdx
    syscall
    cmp $1, %rax
    jl fail_close

    mov $3, %rax
    mov %r12, %rdi
    syscall

    mov $1, %rax
    mov $1, %rdi
    lea ok_message(%rip), %rsi
    mov $ok_message_end - ok_message, %rdx
    syscall

    mov $60, %rax
    xor %rdi, %rdi
    syscall

fail_close:
    mov $3, %rax
    mov %r12, %rdi
    syscall
fail:
    mov $1, %rax
    mov $1, %rdi
    lea fail_message(%rip), %rsi
    mov $fail_message_end - fail_message, %rdx
    syscall

    mov $60, %rax
    mov $1, %rdi
    syscall

.section .rodata
root_path:
    .asciz "/"
ok_message:
    .ascii "klcs-getdents: OK\n"
ok_message_end:
fail_message:
    .ascii "klcs-getdents: FAIL\n"
fail_message_end:

.section .bss
.align 16
dent_buffer:
    .skip 512
