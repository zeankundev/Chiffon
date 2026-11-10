#include "kernel.h"

extern const uint8_t _initrd_start[], _initrd_end[];

static const char *on_exit_policy = "poweroff";

void kernel_poweroff(void) {
    int r = persist_sync_all();
    if (r) kprintf("[persist] warning: some changes were not saved\n");
    board_poweroff();
}
static int respawns;
static uint64_t respawn_t0;

#define NENVSLOTS 48
struct envctx { char *v[NENVSLOTS]; int n; };
static void env_cb(const char *k, const char *val, void *c) {
    struct envctx *e = c;
    size_t l = strlen(k) + strlen(val) + 2;
    char *s = kmalloc(l);
    if (!s || e->n >= 38) return;
    ksnprintf(s, l, "%s=%s", k, val);
    e->v[e->n++] = s;
}
static int has_env(struct envctx *e, const char *k) {
    size_t l = strlen(k);
    for (int i = 0; i < e->n; i++) if (!strncmp(e->v[i], k, l) && e->v[i][l] == '=') return 1;
    return 0;
}

static int screen_started;                           /* the framebuffer console shell is started once */

static int start_init(void) {
    const char* path = cmdline_get("init");
    if (!path) path = cfg_get("init", "path", 0);
    static const char *search[] = { "/sbin/init", "/bin/init", "/etc/init", "/bin/busybox", 0 };
    const char *console = cfg_get("init", "console", "/dev/console");
    on_exit_policy = cfg_get("init", "on_exit", "poweroff");

    struct envctx env; env.n = 0;
    cfg_foreach("env", env_cb, &env);
    if (!has_env(&env, "PATH")) env.v[env.n++] = "PATH=/bin:/sbin:/usr/bin:/usr/sbin";
    if (!has_env(&env, "HOME")) env.v[env.n++] = "HOME=/";
    if (!has_env(&env, "TERM")) env.v[env.n++] = "TERM=vt100";
    env.v[env.n] = 0;

    static char args[256];
    strlcpy(args, cfg_get("init", "args", ""), sizeof args);
    /* An explicit path is tried alone, except /bin/sh: if the image has a BusyBox but the shell link
     * is missing, the BusyBox multicall binary runs the same shell (`busybox sh`). */
    static const char *configured[3];
    configured[0] = path; configured[1] = 0; configured[2] = 0;
    if (path && !strcmp(path, "/bin/sh")) configured[1] = "/bin/busybox";
    const char **cands = path ? configured : search;
    int r = -ENOENT;
    for (int i = 0; ; i++) {
        const char *cand = cands[i];
        if (!cand) break;
        int fallback = path && i > 0;                    /* the BusyBox stand-in for a missing /bin/sh */
        int shell = (!path || fallback) && (!strcmp(cand, "/bin/sh") || !strcmp(cand, "/bin/busybox"));
        char *argv[20]; int n = 0;
        argv[n++] = (char *)cfg_get("init", "argv0", shell ? "-sh" : cand);
        if (!strcmp(cand, "/bin/busybox") && (!path || fallback)) argv[n++] = "sh";
        for (char *p = args; *p && n < 18;) {
            while (*p == ' ') p++;
            if (!*p) break;
            argv[n++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = 0;
        }
        argv[n] = 0;
        r = proc_spawn_init(cand, argv, env.v, console);
        if (r > 0) {
            for (int j = 0; j < NPROC; j++)
                if (ptable[j].pid == r) strlcpy(ptable[j].cwd, cfg_get("init", "cwd", "/"), PATH_MAX);
            kprintf("init: pid %d is %s\n", r, cand);
            /* A framebuffer is a console too: start the same shell on /dev/tty1 (the window).  Its environment
             * says which terminal it is on (CHIFFON_CONSOLE), for programs that behave differently there. */
            if (!screen_started && fb_primary() && strcmp(console, "/dev/tty1") != 0) {
                screen_started = 1;
                char *senv[NENVSLOTS + 1]; int m = 0;
                for (int k = 0; k < env.n && m < NENVSLOTS - 1; k++) senv[m++] = env.v[k];
                senv[m++] = "CHIFFON_CONSOLE=/dev/tty1";
                senv[m] = 0;
                int sr = proc_spawn_console(cand, argv, senv, "/dev/tty1");
                if (sr > 0) {
                    for (int j = 0; j < NPROC; j++)
                        if (ptable[j].pid == sr) strlcpy(ptable[j].cwd, cfg_get("init", "cwd", "/"), PATH_MAX);
                    kprintf("console: pid %d is %s on /dev/tty1\n", sr, cand);
                } else kprintf("console: no shell on /dev/tty1 (error %d)\n", -sr);
            }
            return r;
        }
        if (path && !cands[i + 1]) { kprintf("init: cannot start %s (error %d)\n", path, -r); break; }
        if (path) kprintf("init: cannot start %s (error %d), trying %s\n", cand, -r, cands[i + 1]);
        strlcpy(args, cfg_get("init", "args", ""), sizeof args);   /* args were split in place */
    }
    return r;
}

void init_exited(int wstatus) {
    kprintf("[init] exited with status %d\n", wstatus);
    if (!kstrcasecmp(on_exit_policy, "respawn")) {
        if (!respawns++) respawn_t0 = ticks;
        if (respawns > 5 && ticks - respawn_t0 < 10 * HZ) kprintf("[init] respawned too many times, bailing out\n");
        else if (start_init() > 0) return;
    } else if (!kstrcasecmp(on_exit_policy, "halt")) {
        kprintf("[chiffon] system halted\n");
        arch_halt();
    }
    kprintf("[chiffon] system powering off\n");
    kernel_poweroff();
}

static void load_config(void) {
    const char *explicit = cmdline_get("ini");
    const char *paths[] = { explicit ? explicit : "/etc/chiffon.ini", "/chiffon.ini", 0 };
    for (int i = 0; paths[i]; i++) {
        char abs[PATH_MAX];
        if (vfs_resolve(paths[i], 1, abs)) continue;
        struct node *n = node_find(abs);
        if (!n || node_load(n) || !n->data) continue;
        cfg_load((const char *)n->data, n->size);
        kprintf("config: %s\n", abs);
        return;
    }
    kprintf("[config] chiffon.ini not found\n");
}

void kmain(uintptr_t bootinfo) {
    smp_bsp_init();                                 /* per-CPU pointer + the kernel lock (see smp.h) */
    board_early_init(bootinfo);
    cmdline_set(board_cmdline());
    kprintf("Chiffon %s (%s, %s)\n", VERSION, VERNAME, ARCH_NAME);

    uintptr_t s, e;
    board_mem_range(&s, &e);
    size_t heap = (e - s) / 3;
    if (heap > CHI_HEAP_MAX) heap = CHI_HEAP_MAX;
    heap &= ~(PAGE_SIZE - 1);
    heap_init(P2V(s), heap);
    pmm_init(s + heap, e);
    kprintf("mem: %lu KiB free for processes, %lu KiB kernel heap\n", (unsigned long)(pmm_free_count() * PAGE_SIZE >> 10), (unsigned long)(heap >> 10));

    arch_init();
    board_init();
    ipc_init();
    console_services_init();
    drivers_init();                                  /* PCI, keyboard, display, ... (bound to the console + IPC services) */
    tty_init();
    vfs_init();

    const char *root = cmdline_get("root");
    if (!root) root = "auto";
    int mounted = 0;
    if (kstrcasecmp(root, "initramfs")) {
        int part = (!strncmp(root, "disk", 4) && root[4] >= '1' && root[4] <= '4') ? root[4] - '0' : 0;
        int r = blk_mount_root(part);
        if (r == 0) mounted = 1;
        else kprintf("root: no usable disk volume (%d), falling back to initramfs\n", -r);
    }
    if (!mounted) vfs_load_cpio(_initrd_start, _initrd_end - _initrd_start);
    vfs_finish();

    load_config();
    if (start_init() <= 0) 
        panic("no init process could be started. Either provide a root disk with /sbin/init, configuring [init] section in chiffon.ini, or use a valid initramfs");
    kprintf("\n");
    smp_init();                                     /* start the other CPUs; they join the scheduler */
    scheduler();
}