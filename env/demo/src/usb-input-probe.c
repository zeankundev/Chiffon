/* The host sends 'a' and horizontal pointer movement after the READY marker. */
#include "libchiffon.h"
int main(int argc, char **argv, char **envp) {
    int bus = chiffon_ipc_lookup(IPC_SVC_HID_BUS);
    struct chi_ipc_msg req = { .tag = HID_BUS_HEAD }, rep;
    if (chiffon_ipc_create(IPC_SVC_HID) != -1 || bus <= 0 ||
        chiffon_ipc_call(bus, &req, &rep) || rep.tag != -1) return 1;
    int fd = chiffon_open("/dev/input/event0", 0, 0);
    if (fd < 0) return 1;
    chiffon_puts("READY USB input\n");
    int down = 0, up = 0, motion = 0;
    while (!up || !motion) {
        struct chi_input_event ev;
        if (chiffon_read(fd, &ev, sizeof ev) != sizeof ev) return 1;
        if (ev.type == CHI_EV_KEY && ev.code == 0x1e) {
            if (ev.value) down = 1;
            else if (down) up = 1;
        }
        if (ev.type == CHI_EV_REL && ev.code == 0 && ev.value == 7) motion = 1;
    }
    chiffon_puts("PASS USB input\n");
    return 0;
}
