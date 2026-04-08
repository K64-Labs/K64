// k64_irq.S – IRQ0 / IRQ1 stubs
.section .text
.code64

.extern k64_sched_handle_timer
.extern k64_keyboard_on_interrupt

.macro PUSH_ALL
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
.endm

.macro POP_ALL
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax
.endm

.global k64_irq0_stub
k64_irq0_stub:
    PUSH_ALL
    movq %rsp, %rdi
    call k64_sched_handle_timer
    movq %rax, %rsp
    POP_ALL
    iretq

.global k64_irq1_stub
k64_irq1_stub:
    PUSH_ALL
    call k64_keyboard_on_interrupt
    POP_ALL
    iretq
