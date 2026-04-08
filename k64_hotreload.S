.section .text
.code64
.global k64_hotreload_trampoline_start
.global k64_hotreload_trampoline_end

.set PLAN_OP_COUNT, 0
.set PLAN_OPS_PTR, 8
.set PLAN_MB_MAGIC_ADDR, 16
.set PLAN_MB_INFO_ADDR, 24
.set PLAN_MB_MAGIC_VALUE, 32
.set PLAN_MB_INFO_VALUE, 40
.set PLAN_ENTRY, 48
.set PLAN_STACK_TOP, 56

.set OP_DST, 0
.set OP_SRC, 8
.set OP_COPY_SIZE, 16
.set OP_ZERO_SIZE, 24
.set OP_SIZE, 32

k64_hotreload_trampoline_start:
    cli
    mov %rdi, %rbx
    mov PLAN_STACK_TOP(%rdi), %rsp
    xor %rbp, %rbp
    xor %r12, %r12
    mov PLAN_OP_COUNT(%rdi), %r12
    mov PLAN_OPS_PTR(%rdi), %r13

1:
    test %r12, %r12
    je 2f
    mov OP_DST(%r13), %rdi
    mov OP_SRC(%r13), %rsi
    mov OP_COPY_SIZE(%r13), %rcx
    rep movsb
    mov OP_ZERO_SIZE(%r13), %rcx
    xor %rax, %rax
    rep stosb
    add $OP_SIZE, %r13
    dec %r12
    jmp 1b

2:
    mov PLAN_MB_MAGIC_ADDR(%rbx), %rax
    mov PLAN_MB_MAGIC_VALUE(%rbx), %edx
    mov %edx, (%rax)
    mov PLAN_MB_INFO_ADDR(%rbx), %rax
    mov PLAN_MB_INFO_VALUE(%rbx), %edx
    mov %edx, (%rax)
    pushq $0
    jmp *PLAN_ENTRY(%rbx)

k64_hotreload_trampoline_end:
