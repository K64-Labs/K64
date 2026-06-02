.section .text
.global _start
_start:
    movl $1, pollfd(%rip)
    movw $4, pollfd+4(%rip)
    movw $0, pollfd+6(%rip)

    mov $7, %rax
    lea pollfd(%rip), %rdi
    mov $1, %rsi
    xor %rdx, %rdx
    syscall
    cmp $1, %rax
    jne fail

    movzwl pollfd+6(%rip), %eax
    test $4, %eax
    jz fail

    mov $1, %rax
    mov $1, %rdi
    lea ok_message(%rip), %rsi
    mov $ok_message_end - ok_message, %rdx
    syscall
    mov $60, %rax
    xor %rdi, %rdi
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
ok_message:
    .ascii "klcs-poll: OK\n"
ok_message_end:
fail_message:
    .ascii "klcs-poll: FAIL\n"
fail_message_end:

.section .data
.align 8
pollfd:
    .long 0
    .short 0
    .short 0
