/* longmode.s – 64-bit entry */

.section .text
.code64
.global long_mode_entry

.extern gdt64_descriptor
.extern k64_kernel_main

.section .text
long_mode_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs

    lea k64_stack_top(%rip), %rsp

    call k64_kernel_main

1:
    hlt
    jmp 1b

.section .bss
.align 16
k64_stack_bottom:
    .skip 16384
k64_stack_top:
