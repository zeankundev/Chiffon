    .section .initrd, "a"
    .balign 4096
    .globl _initrd_start, _initrd_end
_initrd_start:
    .incbin INITRD_FILE
_initrd_end:
    .balign 4