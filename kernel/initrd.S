    .section .initrd, "a"
    .balign 4096
    .globl _initrd_start, _initrd_end
_initrd_start:
    .incbin INITRD_FILE
_initrd_end:
    .balign 4
    .globl _driver_loader_start, _driver_loader_end, _fb_server_start, _fb_server_end
_driver_loader_start:
#ifdef DRIVER_LOADER_FILE
    .incbin DRIVER_LOADER_FILE
#endif
_driver_loader_end:
    .balign 4
_fb_server_start:
#ifdef FB_SERVER_FILE
    .incbin FB_SERVER_FILE
#endif
_fb_server_end:
    .balign 4
