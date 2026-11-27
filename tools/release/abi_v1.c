/* Release baseline for the published freestanding Chiffon ABI on LP64. */
#include "uapi/chiffon_abi.h"

#define ABI_VALUE(symbol, expected) _Static_assert((symbol) == (expected), #symbol)
#define ABI_SIZE(type, expected) ABI_VALUE(sizeof(struct type), expected)
#define ABI_OFFSET(type, member, expected) \
    _Static_assert(__builtin_offsetof(struct type, member) == (expected), #type "." #member)

ABI_VALUE(sizeof(long), 8);
ABI_VALUE(CSYS_ipc_create, 1000);
ABI_VALUE(CSYS_ipc_lookup, 1001);
ABI_VALUE(CSYS_ipc_close, 1002);
ABI_VALUE(CSYS_ipc_send, 1003);
ABI_VALUE(CSYS_ipc_recv, 1004);
ABI_VALUE(CSYS_ipc_call, 1005);
ABI_VALUE(CSYS_ipc_reply, 1006);
ABI_VALUE(CSYS_cap_grant, 1010);
ABI_VALUE(CSYS_dev_map, 1011);
ABI_VALUE(CSYS_irq_claim, 1012);
ABI_VALUE(CSYS_irq_wait, 1013);
ABI_VALUE(CSYS_dev_info, 1014);
ABI_VALUE(CSYS_net_info, 1020);
ABI_VALUE(CSYS_net_ping, 1021);
ABI_VALUE(CSYS_net_resolve, 1022);
ABI_VALUE(CSYS_net_connect, 1023);
ABI_VALUE(CSYS_net_send, 1024);
ABI_VALUE(CSYS_net_recv, 1025);
ABI_VALUE(CSYS_net_close, 1026);

ABI_VALUE(CHI_CAP_MMIO, 1);
ABI_VALUE(CHI_CAP_IRQ, 2);
ABI_VALUE(CHI_CAP_DRIVER, 4);
ABI_VALUE(CHI_CAP_LOADER, 8);
ABI_VALUE(DEV_TYPE_FB, 1);
ABI_VALUE(DEV_TYPE_UART, 2);
ABI_VALUE(IPC_NONBLOCK, 1);
ABI_VALUE(IPC_NAME_MAX, 24);
ABI_VALUE(IPC_DATA_MAX, 48);
ABI_SIZE(chi_ipc_msg, 64);
ABI_OFFSET(chi_ipc_msg, src, 0);
ABI_OFFSET(chi_ipc_msg, dst, 4);
ABI_OFFSET(chi_ipc_msg, tag, 8);
ABI_OFFSET(chi_ipc_msg, len, 12);
ABI_OFFSET(chi_ipc_msg, data, 16);

ABI_SIZE(chi_dev_fb_info, 32);
ABI_OFFSET(chi_dev_fb_info, size, 8);
ABI_OFFSET(chi_dev_fb_info, width, 16);
ABI_OFFSET(chi_dev_fb_info, bpp, 28);
ABI_SIZE(chi_dev_uart_info, 24);
ABI_OFFSET(chi_dev_uart_info, irq, 16);
ABI_SIZE(chi_netinfo, 24);
ABI_OFFSET(chi_netinfo, mac, 16);
ABI_OFFSET(chi_netinfo, configured, 22);

ABI_VALUE(CHI_FBIOGET_VSCREENINFO, 0x4600);
ABI_VALUE(CHI_FBIOPUT_VSCREENINFO, 0x4601);
ABI_VALUE(CHI_FBIOGET_FSCREENINFO, 0x4602);
ABI_VALUE(CHI_FBIOPAN_DISPLAY, 0x4606);
ABI_VALUE(CHI_FBIOBLANK, 0x4611);
ABI_VALUE(CHI_KDSETMODE, 0x4B3A);
ABI_VALUE(CHI_KDGETMODE, 0x4B3B);
ABI_VALUE(CHI_KD_TEXT, 0);
ABI_VALUE(CHI_KD_GRAPHICS, 1);
ABI_SIZE(chi_fb_var_screeninfo, 160);
ABI_OFFSET(chi_fb_var_screeninfo, red, 32);
ABI_OFFSET(chi_fb_var_screeninfo, reserved, 144);
ABI_SIZE(chi_fb_fix_screeninfo, 80);
ABI_OFFSET(chi_fb_fix_screeninfo, smem_start, 16);
ABI_OFFSET(chi_fb_fix_screeninfo, line_length, 48);
ABI_OFFSET(chi_fb_fix_screeninfo, mmio_start, 56);

ABI_VALUE(CHI_KBDSETMODE, 0x4B80);
ABI_VALUE(CHI_KBDGETMODE, 0x4B81);
ABI_VALUE(CHI_KBDMODE_TTY, 0);
ABI_VALUE(CHI_KBDMODE_DIRECT, 1);
ABI_VALUE(CHI_EV_KEY, 1);
ABI_VALUE(CHI_EV_REL, 2);
ABI_VALUE(CHI_EV_BTN, 3);
ABI_SIZE(chi_input_event, 8);
ABI_OFFSET(chi_input_event, type, 0);
ABI_OFFSET(chi_input_event, code, 2);
ABI_OFFSET(chi_input_event, value, 4);
