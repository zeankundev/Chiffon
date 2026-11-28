# User page protection

Chiffon sets user read, write and execute permissions on individual 4 KiB pages on AArch64 and x86_64. AArch64 uses the EL0 access, read-only and UXN descriptor bits. x86_64 uses the user, read/write and NX bits and requires processor NX support at boot. Any accessible page is readable on these mappings. `PROT_NONE` removes user access while keeping the page and its contents available for a later protection change.

The ELF loader checks the program-header and segment bounds and requires the entry point to lie in an executable load segment. It derives page permissions from the `PT_LOAD` flags for both the main image and interpreter. Adjacent segments may share a page when their combined permissions satisfy W^X. The initial stack, heap growth and default device mappings are writable and non-executable. The initial stack stays non-executable when an ELF requests an executable stack.

`mmap` and `mprotect` reject simultaneous write and execute access. `mprotect` changes existing pages in place and checks the full range before updating it. Device mappings reject execute access and retain their memory attributes across protection changes. Fork copies ordinary page contents and protections and skips device mappings. The child must obtain authority to map a device into its own address space. Processes with driver authority or an active driver bootstrap cannot create threads sharing their address space. [Memory management](../kernel/proc.c), [syscalls](../kernel/syscall.c)

The protection applies to user mappings. The trusted kernel uses its direct map to populate new file-backed pages and synchronize instruction caches. The Demo VM probe exercised W^X request denial, permission transitions and private fork contents in both QEMU guests. The capability probe checked ordinary-process MMIO denial and invalid IPC buffers. The HID service receives copied reports and requires no controller mapping. Kernel mapping protections, cross-CPU synchronization and comprehensive user-pointer coverage still need separate review. [Tested scope](RELEASE_EVIDENCE.md)

## Kernel stack guard and random bytes

The kernel is built with `-fstack-protector-strong`. Boot code initializes the global stack guard before entering C kernel startup. AArch64 uses RNDR when the CPU advertises it, and x86_64 uses RDRAND when available. If a usable random word is unavailable at boot, the guard keeps its fixed terminator value. That fallback can detect some accidental stack overwrites and is predictable. A failed guard check panics the kernel. [Build flags](../Makefile), [guard implementation](../kernel/hardening.c)

`getrandom` reads the same hardware sources and has no software entropy fallback. If the source is unavailable before any bytes are read, it returns `-ENOSYS` for an unsupported instruction or `-EAGAIN` when the instruction fails its bounded retry loop. It returns the actual byte count if a source failure follows some output. [Random syscall](../kernel/syscall.c)

The [release smoke probe](RELEASE_EVIDENCE.md) checks anonymous mappings, protection syscall results, private data after fork, and unmapping. Its checks cover data reads and writes and syscall return values. Execution faults, device-map inheritance, and stack-canary behavior need separate evidence.
