.section .text
.code64
.global k64_vmm_call_asm

k64_vmm_call_asm:
    pushfq
    push %r12
    push %r13
    cli
    mov %cr3, %r12
    mov %rsp, %r13
    mov %rdi, %cr3
    mov %rsi, %rsp
    and $-16, %rsp
    sub $8, %rsp
    mov %rdx, %rax
    mov %rcx, %rdi
    mov %r8, %rsi
    mov %r9, %rdx
    call *%rax
    mov %r13, %rsp
    mov %r12, %cr3
    pop %r13
    pop %r12
    popfq
    ret
