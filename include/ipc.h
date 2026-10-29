/* Message passing between processes and kernel services.
 *
 * Ports are named, owned by a process (or by the kernel for built-in services) and hold a small
 * queue of 64-byte messages.  Two kinds:
 *   - user ports:    a server process ipc_recv()s requests and ipc_reply()s to callers
 *   - kernel ports:  a handler function runs synchronously in the caller's context (drivers use
 *                    this to export their functionality without a context switch)
 * Both look identical to a client: lookup by name, then send/call. */
#pragma once
#include "uapi/chiffon_abi.h"

typedef int (*ipc_handler)(const struct chi_ipc_msg *req, struct chi_ipc_msg *rep);   /* return 0 or -errno */

struct proc;
void ipc_init(void);
int  ipc_register_service(const char *name, ipc_handler h);     /* kernel-side server; -errno or port id */
long ipc_syscall(long nr, long a0, long a1, long a2);
void ipc_proc_exit(struct proc *p);                              /* drop ports, fail pending calls */
size_t ipc_dump(char *buf, size_t n);
