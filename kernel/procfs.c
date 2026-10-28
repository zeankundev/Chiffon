#include "kernel.h"

static int listed[NPROC];

static void put(const char *path, const char *text) {
    struct node *n = node_find(path);
    if (!n) n = node_create(path, S_IFREG | 0444);
    if (!n) return;
    size_t l = strlen(text);
    if (!n->is_static) kfree(n->data);
    n->data = kmalloc(l + 1); n->is_static = 0;
    if (!n->data) { n->size = n->cap = 0; return; }
    memcpy(n->data, text, l); n->size = l; n->cap = l + 1;
    n->mode = S_IFREG | 0444;
}
static void drop(const char *path) { struct node *n = node_find(path); if (n) node_unlink(n); }

void procfs_refresh(void) {
    char buf[640], path[64];
    if (!node_find("/proc")) node_create("/proc", S_IFDIR | 0555);
    ksnprintf(buf, sizeof buf, "%lu.%02lu %lu.00\n", (unsigned long)(ticks / HZ), (unsigned long)(ticks % HZ), (unsigned long)(ticks / HZ));
    put("/proc/uptime", buf);
    put("/proc/loadavg", "0.00 0.00 0.00 1/1 1\n");
    unsigned long tot = pmm_total_count() * 4, fr = pmm_free_count() * 4;
    ksnprintf(buf, sizeof buf, "MemTotal:       %8lu kB\nMemFree:        %8lu kB\nMemAvailable:   %8lu kB\nBuffers:               0 kB\nCached:                0 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n", tot, fr, fr);
    put("/proc/meminfo", buf);
    put("/proc/version", "Chiffon " VERSION " (" ARCH_NAME ")\n");

    int alive[NPROC] = {0};
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p->state == P_UNUSED || p->state == P_EMBRYO) continue;
        alive[i] = p->pid;
        char st = p->state == P_RUNNING || p->state == P_RUNNABLE ? 'R' : p->state == P_ZOMBIE ? 'Z' : 'S';
        unsigned long vsz = 0;
        for (int v = 0; v < p->mm.nvma; v++) vsz += p->mm.vma[v].end - p->mm.vma[v].start;
        ksnprintf(path, sizeof path, "/proc/%d", p->pid);
        if (!node_find(path)) node_create(path, S_IFDIR | 0555);
        ksnprintf(buf, sizeof buf, "%d (%s) %c %d %d %d 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 0 %lu %lu 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                  p->pid, p->name, st, p->ppid, p->pgid, p->pgid, vsz, vsz / 4096);
        ksnprintf(path, sizeof path, "/proc/%d/stat", p->pid); put(path, buf);
        ksnprintf(buf, sizeof buf, "Name:\t%s\nState:\t%c\nPid:\t%d\nPPid:\t%d\nUid:\t0\t0\t0\t0\nVmSize:\t%lu kB\n", p->name, st, p->pid, p->ppid, vsz / 1024);
        ksnprintf(path, sizeof path, "/proc/%d/status", p->pid); put(path, buf);
        ksnprintf(path, sizeof path, "/proc/%d/cmdline", p->pid);
        { char c[20]; ksnprintf(c, sizeof c, "%s", p->name); put(path, c); }
    }
    for (int i = 0; i < NPROC; i++) {
        if (!listed[i] || listed[i] == alive[i]) continue;
        int still = 0;
        for (int j = 0; j < NPROC; j++) if (alive[j] == listed[i]) still = 1;
        if (still) continue;
        const char *f[] = {"stat", "status", "cmdline"};
        for (int k = 0; k < 3; k++) { ksnprintf(path, sizeof path, "/proc/%d/%s", listed[i], f[k]); drop(path); }
        ksnprintf(path, sizeof path, "/proc/%d", listed[i]); drop(path);
    }
    for (int i = 0; i < NPROC; i++) listed[i] = alive[i];
}