.section .text
.code64
.global k64_vmm_call_asm

k64_vmm_call_asm:
    pushfq
    cli
    mov %cr3, %r10
    mov %rsp, %r11
    mov %rdi, %cr3
    mov %rsi, %rsp
    and $-16, %rsp
    sub $8, %rsp
    mov %rdx, %rax
    mov %rcx, %rdi
    mov %r8, %rsi
    mov %r9, %rdx
    call *%rax
    mov %r11, %rsp
    mov %r10, %cr3
    popfq
    ret
