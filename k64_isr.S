// k64_isr.S – exception stubs 0–31
.section .text
.code64

.extern k64_exception_handler

/* no-error-code */
.macro ISR_NOERR idx
.global k64_isr_stub\idx
k64_isr_stub\idx:
    movq $\idx, %rdi       /* vec */
    movq $0,   %rsi        /* err */
    movq 0(%rsp), %rdx     /* rip */
    movq 8(%rsp), %rcx     /* cs */
    movq 16(%rsp), %r8     /* rflags */
    call k64_exception_handler
1:
    hlt
    jmp 1b
.endm

/* with error code */
.macro ISR_ERR idx
.global k64_isr_stub\idx
k64_isr_stub\idx:
    movq $\idx, %rdi       /* vec */
    movq 0(%rsp), %rsi     /* err */
    movq 8(%rsp), %rdx     /* rip */
    movq 16(%rsp), %rcx    /* cs */
    movq 24(%rsp), %r8     /* rflags */
    call k64_exception_handler
1:
    hlt
    jmp 1b
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31
