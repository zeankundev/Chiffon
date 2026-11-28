#include "libchiffon.h"

static int fail(const char *check) {
    chiffon_puts("FAIL cap-probe ");
    chiffon_puts(check);
    chiffon_puts("\n");
    return 1;
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;
    const long denied = -1, fault = -14, invalid = -22;
    int pid = (int)ksys(CF_getpid, 0, 0, 0, 0, 0, 0);
    unsigned types[] = {CHI_CAP_LOADER, CHI_CAP_MMIO, CHI_CAP_IRQ,
                        CHI_CAP_DRIVER, CHI_CAP_DMA};
    for (unsigned i = 0; i < sizeof types / sizeof types[0]; i++)
        if (chiffon_cap_grant(pid, types[i], 0, 0) != denied)
            return fail("self grant");
    if (chiffon_ipc_create(IPC_SVC_FB) != denied ||
        chiffon_ipc_create(IPC_SVC_LOADER) != denied ||
        chiffon_ipc_create(IPC_SVC_CONSOLE) != denied)
        return fail("reserved service");
    if (chiffon_irq_claim(1) != denied || chiffon_irq_wait(1) != denied ||
        chiffon_dev_map(4096, 4096) != denied)
        return fail("device authority");
    if (chiffon_dev_info(DEV_TYPE_FB, 0) != fault)
        return fail("device info output");
    long port = chiffon_ipc_create("probe.cap");
    if (port < 0) return fail("private port");
    struct chi_ipc_msg msg = { .len = IPC_DATA_MAX + 1 };
    if (chiffon_ipc_send(port, &msg, 0) != invalid ||
        chiffon_ipc_send(port, 0, 0) != fault ||
        chiffon_ipc_recv(port, 0, 0) != fault)
        return fail("message bounds");
    if (chiffon_ipc_close(port)) return fail("close");
    chiffon_puts("PASS cap-probe\n");
    return 0;
}
