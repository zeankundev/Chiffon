#include "libchiffon.h"

int main(int argc, char **argv, char **envp) {
    (void)argv;
    (void)envp;
    int fd = chiffon_open("/dev/tty0", 0, 0);
    int mode = -1;
    if (fd < 0 || chiffon_ioctl(fd, CHI_KBDGETMODE, &mode) || mode != CHI_KBDMODE_TTY)
        return 1;
    if (argc > 1) {
        chiffon_puts("PASS restored TTY\n");
        return 0;
    }
    if (chiffon_ioctl(fd, CHI_KBDSETMODE, (void *)CHI_KBDMODE_DIRECT) ||
        chiffon_ioctl(fd, CHI_KBDGETMODE, &mode) || mode != CHI_KBDMODE_DIRECT)
        return 1;
    chiffon_puts("READY direct Ctrl-C\n");
    const unsigned short codes[] = {0x1d, 0x2e, 0x2e, 0x1d};
    for (unsigned i = 0; i < 4; i++) {
        struct chi_input_event event;
        if (chiffon_read(fd, &event, sizeof event) != sizeof event ||
            event.type != CHI_EV_KEY || event.code != codes[i] || event.value != (i < 2))
            return 1;
    }
    chiffon_puts("PASS direct Ctrl-C\n");
    return 0;
}
