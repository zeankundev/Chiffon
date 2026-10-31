/* Message passing.  See include/ipc.h for the model. */
#include "kernel.h"

#define NPORT 32
#define QLEN 16
struct port {
    int used, owner;                         /* owner: pid, 0 = kernel */
    ipc_handler svc;                         /* kernel service: run synchronously in the caller */
    char name[IPC_NAME_MAX];
    int head, count;
    struct chi_ipc_msg q[QLEN];
};
static struct port ports[NPORT];

void ipc_init(void) { memset(ports, 0, sizeof ports); }

static struct port *by_id(long id) { return id >= 1 && id <= NPORT && ports[id - 1].used ? &ports[id - 1] : 0; }
static int self(void) { return cur ? cur->pid : 0; }
static int name_ok(const char *n) { size_t l = strlen(n); return l > 0 && l < IPC_NAME_MAX; }

static long port_new(const char *name, int owner, ipc_handler svc) {
    if (!name_ok(name)) return -EINVAL;
    for (int i = 0; i < NPORT; i++) {
        if (ports[i].used && !strcmp(ports[i].name, name)) {
            /* If it's a kernel service and caller has CHI_CAP_DRIVER, allow driver takeover */
            if (ports[i].svc && cap_check(cur, CHI_CAP_DRIVER, 0, 0)) {
                ports[i].svc = 0;
                ports[i].owner = owner;
                ports[i].head = 0;
                ports[i].count = 0;
                return i + 1;
            }
            return -EEXIST;
        }
    }
    for (int i = 0; i < NPORT; i++) {
        if (ports[i].used) continue;
        memset(&ports[i], 0, sizeof ports[i]);
        ports[i].used = 1; ports[i].owner = owner; ports[i].svc = svc;
        strlcpy(ports[i].name, name, IPC_NAME_MAX);
        return i + 1;
    }
    return -ENOMEM;
}
int ipc_register_service(const char *name, ipc_handler h) { return (int)port_new(name, 0, h); }

/* fail every caller blocked on port `pi` (the server is gone) */
static void fail_waiters(int pi) {
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p->state == P_UNUSED || !p->ipc_waiting || p->ipc_wait_port != pi + 1) continue;
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
        pt = &ports[id - 1];
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
        for (int i = 0; i < NPORT; i++) if (ports[i].used && !strcmp(ports[i].name, name)) return i + 1;
        return -ENOENT;
    case CSYS_ipc_close: {
        struct port *pt = by_id(a0);
        if (!pt) return -ENOENT;
        if (pt->svc || pt->owner != cur->pid) return -EPERM;
        port_drop(a0 - 1);
        return 0; }
    case CSYS_ipc_send:
        if (!uok(a1, sizeof m)) return -EFAULT;
        memcpy(&m, (void *)a1, sizeof m);
        return ipc_send_k(a0, &m, (int)a2);
    case CSYS_ipc_recv:
        if (!uok(a1, sizeof m)) return -EFAULT;
        if ((r = ipc_recv_k(a0, &m, (int)a2))) return r;
        memcpy((void *)a1, &m, sizeof m);
        return 0;
    case CSYS_ipc_call: {
        struct chi_ipc_msg rep;
        if (!uok(a1, sizeof m) || !uok(a2, sizeof rep)) return -EFAULT;
        memcpy(&m, (void *)a1, sizeof m);
        if ((r = ipc_call_k(a0, &m, &rep))) return r;
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
        if (ports[i].svc) o += ksnprintf(buf + o, n - o, "%-3d %-24s %-8s %s\n", i + 1, ports[i].name, "kernel", "-");
        else o += ksnprintf(buf + o, n - o, "%-3d %-24s %-8d %d\n", i + 1, ports[i].name, ports[i].owner, ports[i].count);
    }
    return o < n ? o : n - 1;
}
