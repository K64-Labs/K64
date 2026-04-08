/* boot.s – K64 32-bit boot, Multiboot v1, paging, Long Mode switch */

.set MULTIBOOT_MAGIC,    0x1BADB002
.set MULTIBOOT_FLAGS,    0
.set MULTIBOOT_CHECKSUM, -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

.section .multiboot
    .long MULTIBOOT_MAGIC
    .long MULTIBOOT_FLAGS
    .long MULTIBOOT_CHECKSUM

.section .bss
.globl k64_mb_magic
k64_mb_magic: .long 0

.globl k64_mb_info
k64_mb_info: .long 0

.align 16
stack_bottom:
    .skip 4096
stack_top:

.section .text
.code32
.global _start

.extern long_mode_entry

/* Page tables + GDT */
.align 4096
pml4_table:
    .quad 0

.align 4096
pdpt_table:
    .quad 0
    .rept 511
        .quad 0
    .endr

.align 8
gdt64:
    .quad 0x0000000000000000        /* null desc */
    .quad 0x00AF9A000000FFFF        /* code */
    .quad 0x00AF92000000FFFF        /* data */

.globl gdt64_descriptor
gdt64_descriptor:
    .word (gdt64_descriptor_end - gdt64 - 1)
    .long gdt64
gdt64_descriptor_end:

_start:
    cli

    /* Save Multiboot values */
    mov %eax, k64_mb_magic
    mov %ebx, k64_mb_info

    mov $stack_top, %esp

    /* Load the kernel GDT before the long-mode far jump. */
    lgdt gdt64_descriptor

    call setup_paging

    /* Enable PAE */
    mov %cr4, %eax
    or  $(1 << 5), %eax
    mov %eax, %cr4

    /* Enable Long Mode in EFER */
    mov $0xC0000080, %ecx
    rdmsr
    or  $(1 << 8), %eax
    wrmsr

    /* CR3 -> PML4 */
    mov $pml4_table, %eax
    mov %eax, %cr3

    /* Enable paging + protection */
    mov %cr0, %eax
    or  $0x80000001, %eax
    mov %eax, %cr0

    /* Long mode active */
    ljmp $0x08, $long_mode_entry

setup_paging:
    /* Clear PML4 */
    mov $pml4_table, %edi
    mov $0, %eax
    mov $512, %ecx
1:
    mov %eax, (%edi)
    add $8, %edi
    loop 1b

    /* Clear PDPT */
    mov $pdpt_table, %edi
    mov $512, %ecx
2:
    mov %eax, (%edi)
    add $8, %edi
    loop 2b

    /* PML4[0] -> PDPT */
    mov $pml4_table, %edi
    mov $pdpt_table, %eax
    or  $0x003, %eax          /* P,RW */
    mov %eax, (%edi)

    /* PDPT[0] = 1GiB page @0 */
    mov $pdpt_table, %edi
    mov $0x00000000, %eax
    or  $0x083, %eax          /* P,RW,PS */
    mov %eax, (%edi)

    ret
