# User page protection

Chiffon sets user read, write and execute permissions on individual 4 KiB pages on AArch64 and x86_64. AArch64 uses the EL0 access, read-only and UXN descriptor bits. x86_64 uses the user, read/write and NX bits and requires processor NX support at boot. Any accessible page is readable on these mappings. `PROT_NONE` removes user access while keeping the page and its contents available for a later protection change.

The ELF loader checks the program-header and segment bounds and requires the entry point to lie in an executable load segment. It derives page permissions from the `PT_LOAD` flags for both the main image and interpreter. Adjacent segments may share a page when their combined permissions satisfy W^X. The initial stack, heap growth and default device mappings are writable and non-executable. The initial stack stays non-executable when an ELF requests an executable stack.

`mmap` and `mprotect` reject simultaneous write and execute access. `mprotect` changes existing pages in place and checks the full range before updating it. Device mappings reject execute access and keep their device memory attributes across protection changes and fork. Fork copies ordinary page contents and protections and shares mapped device memory. [Memory management](../kernel/proc.c), [syscalls](../kernel/syscall.c)

The protection applies to user mappings. The trusted kernel uses its direct map to populate new file-backed pages and synchronize instruction caches. Kernel mapping protections and cross-CPU page-table synchronization need separate review. Runtime confirmation is pending for both supported architectures.
