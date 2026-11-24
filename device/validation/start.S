.section .boot,"ax"
.code16
boot:
    cli
    inb $0x92, %al
    orb $2, %al
    outb %al, $0x92
    lgdtl %cs:(descriptor - boot)
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    ljmpl $8, $protected
.balign 8
gdt:
    /* Descriptors live in ROM. Set the accessed bits before entering protected mode. */
    .quad 0
    .quad 0x00cf9b000000ffff
    .quad 0x00cf93000000ffff
descriptor:
    .word descriptor - gdt - 1
    .long gdt
.code32
protected:
    movw $16, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl $0x80000, %esp
    cld
    call benchmark
    cli
    hlt
.section .reset,"ax"
.code16
    ljmp $0xf000, $0
.section .note.GNU-stack,"",@progbits
