.section .text
.code64

.global k64_user_enter_asm
k64_user_enter_asm:
    cli
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, 0(%rcx)
    movq %cr3, %rax
    movq %rax, 8(%rcx)
    movq %rdi, %rax
    movq %rax, %cr3
    movq %rsi, %rsp
    pushq $0x1b
    pushq %rsi
    pushq $0x202
    pushq $0x23
    pushq %rdx
    iretq

.global k64_user_return_asm
k64_user_return_asm:
    movq 8(%rdi), %rdx
    movq %rdx, %cr3
    movq %rsi, %rax
    movq 0(%rdi), %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

.extern k64_usermode_syscall_handler
.extern k64_usermode_linux_syscall_handler

.global k64_syscall_stub
k64_syscall_stub:
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rdi
    pushq %rsi
    pushq %rbp
    pushq %rbx
    pushq %rdx
    pushq %rcx
    pushq %rax
    movq %rsp, %rdi
    call k64_usermode_syscall_handler
    movq %rax, 0(%rsp)
    popq %rax
    popq %rcx
    popq %rdx
    popq %rbx
    popq %rbp
    popq %rsi
    popq %rdi
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    iretq

.section .bss
.align 16
k64_linux_syscall_stack:
    .skip 16384
k64_linux_syscall_stack_top:
.align 8
k64_linux_syscall_user_rsp:
    .quad 0
k64_linux_syscall_user_rcx:
    .quad 0
k64_linux_syscall_user_r11:
    .quad 0

.section .text
.code64
.global k64_linux_syscall_stub
k64_linux_syscall_stub:
    cli
    movq %rsp, k64_linux_syscall_user_rsp(%rip)
    movq %rcx, k64_linux_syscall_user_rcx(%rip)
    movq %r11, k64_linux_syscall_user_r11(%rip)
    leaq k64_linux_syscall_stack_top(%rip), %rsp

    pushq k64_linux_syscall_user_rsp(%rip)
    pushq k64_linux_syscall_user_r11(%rip)
    pushq k64_linux_syscall_user_rcx(%rip)
    pushq %r9
    pushq %r8
    pushq %r10
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rax

    movq %rsp, %rdi
    call k64_usermode_linux_syscall_handler

    movq 8(%rsp), %rdi
    movq 16(%rsp), %rsi
    movq 24(%rsp), %rdx
    movq 32(%rsp), %r10
    movq 40(%rsp), %r8
    movq 48(%rsp), %r9
    addq $80, %rsp
    movq k64_linux_syscall_user_rcx(%rip), %rcx
    movq k64_linux_syscall_user_r11(%rip), %r11
    movq k64_linux_syscall_user_rsp(%rip), %rsp
    sysretq
