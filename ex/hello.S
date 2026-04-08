.section .text
.global _start
.type _start, @function

_start:
    mov $42, %eax
    ret
