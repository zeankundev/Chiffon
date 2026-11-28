/* Message passing.  See include/ipc.h for the model. */
#include "kernel.h"

#ifdef CHI_TINY
#define QLEN 4
#else
#define QLEN 16
#endif
struct port {
    int id;
    int used, owner;                         /* owner: pid, 0 = kernel */
    ipc_handler svc;                         /* kernel service: run synchronously in the caller */
    char name[IPC_NAME_MAX];
    int head, count;
    struct chi_ipc_msg q[QLEN];
};
static struct port ports[NPORT];

void ipc_init(void) { memset(ports, 0, sizeof ports); }

static struct port *by_id(long id) {
    if (id <= 0 || id > __INT_MAX__) return 0;
    struct port *p = &ports[(id - 1) % NPORT];
    return p->used && p->id == id ? p : 0;
}
static int self(void) { return cur ? cur->pid : 0; }
static int name_ok(const char *n) { size_t l = strlen(n); return l > 0 && l < IPC_NAME_MAX; }

static long port_new(const char *name, int owner, ipc_handler svc) {
    if (!name_ok(name)) return -EINVAL;
    int framebuffer = !strcmp(name, IPC_SVC_FB);
    if (!svc) {
        if (!strcmp(name, IPC_SVC_HID_BUS)) return -EPERM;
        if (!strcmp(name, IPC_SVC_HID) && !cap_check(cur, CHI_CAP_DRIVER, DEV_TYPE_HID, 0)) return -EPERM;
        if (!strcmp(name, IPC_SVC_CONSOLE)) return -EPERM;
        if (!strcmp(name, IPC_SVC_LOADER) && !cap_check(cur, CHI_CAP_LOADER, 0, 0)) return -EPERM;
        if (framebuffer && !cap_check(cur, CHI_CAP_DRIVER, DEV_TYPE_FB, 0)) return -EPERM;
    }
    for (int i = 0; i < NPORT; i++) {
        if (ports[i].used && !strcmp(ports[i].name, name)) {
            if (ports[i].svc && !svc && framebuffer) {
                if (ports[i].id > __INT_MAX__ - NPORT) return -ENOMEM;
                ports[i].id += NPORT;
                ports[i].svc = 0;
                ports[i].owner = owner;
                ports[i].head = 0;
                ports[i].count = 0;
                return ports[i].id;
            }
            return -EEXIST;
        }
    }
    for (int i = 0; i < NPORT; i++) {
        if (ports[i].used || ports[i].id > __INT_MAX__ - NPORT) continue;
        int id = ports[i].id ? ports[i].id + NPORT : i + 1;
        memset(&ports[i], 0, sizeof ports[i]);
        ports[i].id = id;
        ports[i].used = 1; ports[i].owner = owner; ports[i].svc = svc;
        strlcpy(ports[i].name, name, IPC_NAME_MAX);
        return id;
    }
    return -ENOMEM;
}
int ipc_register_service(const char *name, ipc_handler h) { return (int)port_new(name, 0, h); }

/* fail every caller blocked on port `pi` (the server is gone) */
static void fail_waiters(int pi) {
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p->state == P_UNUSED || !p->ipc_waiting || p->ipc_wait_port != ports[pi].id) continue;
        memset(&p->ipc_rep, 0, sizeof p->ipc_rep);
        p->ipc_rep.tag = -EPIPE;
        p->ipc_have_reply = 1; p->ipc_waiting = 0;
        wakeup(&p->ipc_rep);
    }
}
static void port_drop(int pi) {
    fail_waiters(pi);
    wakeup(&ports[pi]);                      /* a server blocked in recv notices the port is gone */
    ports[pi].used = 0;
}
void ipc_proc_exit(struct proc *p) {
    for (int i = 0; i < NPORT; i++) if (ports[i].used && ports[i].owner == p->pid) port_drop(i);
}

static long run_service(struct port *pt, const struct chi_ipc_msg *req, struct chi_ipc_msg *rep) {
    struct chi_ipc_msg in = *req;
    memset(rep, 0, sizeof *rep);
    in.src = self();
    int r = pt->svc(&in, rep);
    rep->tag = r;
    rep->src = 0;
    return r;
}

static long ipc_send_k(long id, const struct chi_ipc_msg *m, int flags) {
    struct port *pt = by_id(id);
    if (!pt) return -ENOENT;
    if (m->len > IPC_DATA_MAX) return -EINVAL;
    if (pt->svc) { struct chi_ipc_msg rep; return run_service(pt, m, &rep); }
    if (pt->count >= QLEN) return -EAGAIN;
    struct chi_ipc_msg *slot = &pt->q[(pt->head + pt->count++) % QLEN];
    *slot = *m; slot->src = self();
    wakeup(pt);
    return 0;
}

/* Only the transport can stamp a wakeup with kernel origin. */
int ipc_notify_hid(void) {
    for (int i = 0; i < NPORT; i++) {
        struct port *pt = &ports[i];
        if (!pt->used || strcmp(pt->name, IPC_SVC_HID)) continue;
        if (pt->count == QLEN) return -EAGAIN;
        struct chi_ipc_msg *slot = &pt->q[(pt->head + pt->count++) % QLEN];
        *slot = (struct chi_ipc_msg){ .tag = HID_OP_WAKE };
        wakeup(pt);
        return 0;
    }
    return -ENOENT;
}
static long ipc_recv_k(long id, struct chi_ipc_msg *out, int flags) {
    struct port *pt = by_id(id);
    if (!pt || pt->svc) return -ENOENT;
    if (pt->owner != self()) return -EPERM;
    for (;;) {
        if (pt->count) {
            *out = pt->q[pt->head]; pt->head = (pt->head + 1) % QLEN; pt->count--;
            return 0;
        }
        if (flags & IPC_NONBLOCK) return -EAGAIN;
        if (cur->killed) return -EINTR;
        sleep_on(pt);
        if (!by_id(id)) return -EPIPE;       /* closed while we slept */
    }
}

static long ipc_call_k(long id, const struct chi_ipc_msg *req, struct chi_ipc_msg *rep) {
    struct port *pt = by_id(id);
    if (!pt) return -EPIPE;
    if (req->len > IPC_DATA_MAX) return -EINVAL;
    if (pt->svc) { run_service(pt, req, rep); return 0; }   /* rep->tag = handler status */
    if (!cur) return -EINVAL;
    if (pt->owner == cur->pid) return -EINVAL;            /* calling your own port can never be answered */
    if (pt->count >= QLEN) return -EAGAIN;
    cur->ipc_waiting = 1; cur->ipc_have_reply = 0; cur->ipc_wait_port = (int)id;
    struct chi_ipc_msg *slot = &pt->q[(pt->head + pt->count++) % QLEN];
    *slot = *req; slot->src = cur->pid;
    wakeup(pt);
    while (!cur->ipc_have_reply) {
        if (cur->killed) { cur->ipc_waiting = 0; return -EINTR; }
        sleep_on(&cur->ipc_rep);
    }
    *rep = cur->ipc_rep;
    cur->ipc_have_reply = 0;
    return 0;                                               /* rep->tag carries the server's status */
}

static long ipc_reply_k(const struct chi_ipc_msg *r) {
    if (r->len > IPC_DATA_MAX) return -EINVAL;
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p->state == P_UNUSED || p->pid != r->dst) continue;
        if (!p->ipc_waiting) return -ESRCH;
        struct port *pt = by_id(p->ipc_wait_port);
        if (!pt || pt->owner != self()) return -EPERM;      /* only the server of that port may answer */
        p->ipc_rep = *r; p->ipc_rep.src = self();
        p->ipc_have_reply = 1; p->ipc_waiting = 0;
        wakeup(&p->ipc_rep);
        return 0;
    }
    return -ESRCH;
}

/* ---- syscalls: validate user pointers, then call the _k versions ---- */
long ipc_syscall(long nr, long a0, long a1, long a2) {
    char name[IPC_NAME_MAX];
    struct chi_ipc_msg m;
    long r;
    switch (nr) {
    case CSYS_ipc_create:
        if ((r = ustrcpy(name, a0, sizeof name))) return r;
        return port_new(name, cur->pid, 0);
    case CSYS_ipc_lookup:
        if ((r = ustrcpy(name, a0, sizeof name))) return r;
        for (int i = 0; i < NPORT; i++) if (ports[i].used && !strcmp(ports[i].name, name)) return ports[i].id;
        return -ENOENT;
    case CSYS_ipc_close: {
        struct port *pt = by_id(a0);
        if (!pt) return -ENOENT;
        if (pt->svc || pt->owner != cur->pid) return -EPERM;
        port_drop(pt - ports);
        return 0; }
    case CSYS_ipc_send:
        if (!uok(a1, sizeof m)) return -EFAULT;
        memcpy(&m, (void *)a1, sizeof m);
        return ipc_send_k(a0, &m, (int)a2);
    case CSYS_ipc_recv:
        if (!uwok(a1, sizeof m)) return -EFAULT;
        if ((r = ipc_recv_k(a0, &m, (int)a2))) return r;
        if (!uwok(a1, sizeof m)) return -EFAULT;
        memcpy((void *)a1, &m, sizeof m);
        return 0;
    case CSYS_ipc_call: {
        struct chi_ipc_msg rep;
        if (!uok(a1, sizeof m) || !uwok(a2, sizeof rep)) return -EFAULT;
        memcpy(&m, (void *)a1, sizeof m);
        if ((r = ipc_call_k(a0, &m, &rep))) return r;
        if (!uwok(a2, sizeof rep)) return -EFAULT;
        memcpy((void *)a2, &rep, sizeof rep);
        return 0; }
    case CSYS_ipc_reply:
        if (!uok(a0, sizeof m)) return -EFAULT;
        memcpy(&m, (void *)a0, sizeof m);
        return ipc_reply_k(&m);
    }
    return -ENOSYS;
}

size_t ipc_dump(char *buf, size_t n) {
    size_t o = 0;
    o += ksnprintf(buf + o, n - o, "%-3s %-24s %-8s %s\n", "ID", "NAME", "OWNER", "QUEUED");
    for (int i = 0; i < NPORT && o < n; i++) {
        if (!ports[i].used) continue;
        if (ports[i].svc) o += ksnprintf(buf + o, n - o, "%-3d %-24s %-8s %s\n", ports[i].id, ports[i].name, "kernel", "-");
        else o += ksnprintf(buf + o, n - o, "%-3d %-24s %-8d %d\n", ports[i].id, ports[i].name, ports[i].owner, ports[i].count);
    }
    return o < n ? o : n - 1;
}
