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
    movq %rdi, %cr3
    movq %rsi, %rsp
    pushq $0x1b
    pushq %rsi
    pushq $0x2
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
