/* ping HOST: four ICMP echo requests, with round-trip times in milliseconds. */
#include "libchiffon.h"

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc < 2) { chiffon_puts("usage: ping HOST\n"); return 2; }
    unsigned ip;
    long r = chiffon_net_resolve(argv[1], &ip);
    if (r) { chiffon_puts("ping: cannot resolve "); chiffon_puts(argv[1]); chiffon_puts("\n"); return 1; }
    int got = 0;
    for (int seq = 1; seq <= 4; seq++) {
        long rtt = chiffon_net_ping(ip, 2000);
        chiffon_puts("seq="); chiffon_putu(seq); chiffon_puts(" ");
        if (rtt >= 0) { chiffon_puts("reply from "); chiffon_putip(ip); chiffon_puts(" time="); chiffon_putu((unsigned long)rtt / 1000); chiffon_puts("."); chiffon_putu((unsigned long)rtt / 100 % 10); chiffon_puts(" ms\n"); got++; }
        else chiffon_puts("no reply\n");
        chiffon_sleep_ms(300);
    }
    chiffon_puts("--- "); chiffon_putu(got); chiffon_puts(" of 4 replies\n");
    return got ? 0 : 1;
}
