OUTPUT_FORMAT(elf32-i386)
SECTIONS {
    . = 0xf0000;
    .rom : { *(.boot) *(.text*) *(.rodata*) }
    . = 0xffff0;
    .reset : { *(.reset) }
    . = 0xfffff;
    .last : { BYTE(0xff) }
    /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
