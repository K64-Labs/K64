.section .text
.global _start
_start:
    mov $83, %rax
    lea dir_path(%rip), %rdi
    mov $0755, %rsi
    syscall
    test %rax, %rax
    js fail

    mov $257, %rax
    mov $-100, %rdi
    lea file_path(%rip), %rsi
    mov $0x241, %rdx
    mov $0644, %r10
    syscall
    test %rax, %rax
    js fail_cleanup_dir
    mov %rax, %r12

    mov $1, %rax
    mov %r12, %rdi
    lea payload(%rip), %rsi
    mov $payload_end - payload, %rdx
    syscall
    cmp $payload_end - payload, %rax
    jne fail_cleanup_file

    mov $3, %rax
    mov %r12, %rdi
    syscall

    mov $332, %rax
    mov $-100, %rdi
    lea file_path(%rip), %rsi
    xor %rdx, %rdx
    mov $0x7ff, %r10
    lea statx_buf(%rip), %r8
    syscall
    test %rax, %rax
    js fail_cleanup_file

    mov $82, %rax
    lea file_path(%rip), %rdi
    lea renamed_path(%rip), %rsi
    syscall
    test %rax, %rax
    js fail_cleanup_file

    mov $87, %rax
    lea renamed_path(%rip), %rdi
    syscall
    test %rax, %rax
    js fail_cleanup_dir

    mov $84, %rax
    lea dir_path(%rip), %rdi
    syscall
    test %rax, %rax
    js fail

    mov $1, %rax
    mov $1, %rdi
    lea ok_message(%rip), %rsi
    mov $ok_message_end - ok_message, %rdx
    syscall
    mov $60, %rax
    xor %rdi, %rdi
    syscall

fail_cleanup_file:
    mov $3, %rax
    mov %r12, %rdi
    syscall
    mov $87, %rax
    lea file_path(%rip), %rdi
    syscall
    mov $87, %rax
    lea renamed_path(%rip), %rdi
    syscall
fail_cleanup_dir:
    mov $84, %rax
    lea dir_path(%rip), %rdi
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
dir_path:
    .asciz "/tmp/klcs-fsops"
file_path:
    .asciz "/tmp/klcs-fsops/file"
renamed_path:
    .asciz "/tmp/klcs-fsops/renamed"
payload:
    .ascii "desktop plumbing\n"
payload_end:
ok_message:
    .ascii "klcs-fsops: OK\n"
ok_message_end:
fail_message:
    .ascii "klcs-fsops: FAIL\n"
fail_message_end:

.section .bss
.align 16
statx_buf:
    .skip 256
