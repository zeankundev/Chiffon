/* netinfo: show the network configuration (configures the NIC on first use). */
#include "libchiffon.h"

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    struct chi_netinfo ni;
    if (chiffon_net_info(&ni)) { chiffon_puts("netinfo: no network interface\n"); return 1; }
    chiffon_puts("ip       "); chiffon_putip(ni.ip); chiffon_puts("\n");
    chiffon_puts("netmask  "); chiffon_putip(ni.mask); chiffon_puts("\n");
    chiffon_puts("gateway  "); chiffon_putip(ni.gateway); chiffon_puts("\n");
    chiffon_puts("dns      "); chiffon_putip(ni.dns); chiffon_puts("\n");
    chiffon_puts("mac      ");
    for (int i = 0; i < 6; i++) {
        static const char hx[] = "0123456789abcdef";
        char b[3] = { hx[ni.mac[i] >> 4], hx[ni.mac[i] & 15], 0 };
        chiffon_puts(b);
        if (i < 5) chiffon_puts(":");
    }
    chiffon_puts("\n");
    return 0;
}
