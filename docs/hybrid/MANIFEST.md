# Hybrid placement manifest

## P0 source inventory

Baseline `b57e409b0489893f1923c485e1605868be047883`. Each row names files relative to its directory. **Core** means privileged implementation or the header governing it. **Modular** means an existing user-space interface or a candidate whose device-specific policy should leave kernel space. Headers carry the placement of the behavior they describe. The rows describe the destination; the last column records relevant baseline behavior and dependencies.

| Directory | Files | Placement | Reason and boundary |
| --- | --- | --- | --- |
| arch/aarch64, arch/x86_64 (each) | arch.c, arch_defs.h, boot.S, entry.S, linker.ld, sysnum.h | Core | Entry, traps, context transitions, architecture VM operations, ABI dispatch and kernel layout must work before a server exists. |
| kernel | main.c, cfg.c, initrd.S, lib.c, hardening.c | Core | Startup, boot configuration, embedded rescue/service images, freestanding runtime and early stack protection. |
| kernel | mm.c, proc.c, smp.c, syscall.c, ipc.c | Core | Frame/page ownership, scheduling and process lifetime, CPU coordination, syscall and IPC dispatch, device capabilities and revocation enforce process isolation. |
| kernel | driver.c, block.c | Core | Ordered platform bring-up, hardware IRQ dispatch and synchronous storage transport remain privileged. |
| kernel | vfs.c, procfs.c, ext.c, fat.c, persist.c | Core | File namespace, process metadata, filesystem parsing and replay share inode and block lifetimes. Keep filesystem semantics and journal ordering here. |
| kernel | net.c, socket.c | Core | Packet processing and endpoint state share buffers with trusted network transports. Application clients use the socket syscall interface. |
| kernel | console.c, tty.c, vt.c, kbd.c, fb.c, fbcon.c, font8x16.c | Core | Early output, emergency console, VT ownership, input routing and primary display access must survive a peripheral service exit. Keep optional source-specific input policy outside this path. |
| drivers/aarch64 | ramfb.c, virtio_input.c, virtio_net.c | Core | QEMU primary framebuffer setup, primary keyboard delivery and trusted NIC transport. Virtio queues and fw_cfg DMA require privileged buffer ownership. |
| drivers/x86_64 | lapic.c, pci.c, ps2_kbd.c, vga_text.c | Core | Platform IRQs, bus discovery, primary keyboard and boot text console. Keep global PCI config access privileged. |
| drivers/x86_64 | bochs_vbe.c, mb_fb.c, vmsvga.c | Core | These are candidate backends for the primary kernel framebuffer on different machines. Retain boot-console selection and mode setup. Secondary drawing services can use a constrained framebuffer grant. |
| drivers/x86_64 | nic.c, e1000.c, virtio_net.c | Core | PCI NIC selection and DMA rings are the local transports for the core network stack. |
| drivers/x86_64 | usb.c | Modular | Currently a kernel driver combining descriptor parsing, HID keyboard/mouse handling and removable-storage attachment. Split class handling from port/endpoint transport. Preserve FAT/block contracts and invalidate pending calls on removal. |
| drivers/x86_64 | xhci.c | Modular | Optional host-controller service candidate. Keep a trusted kernel transport for DMA rings, addresses, quiescence and IRQ acknowledgement until device isolation exists. A restricted server can own protocol decisions through bounded operations. |
| include | arch.h, board.h, bootmem.h, smp.h, kernel.h | Core | Architecture, board, memory bounds, synchronization and shared kernel object contracts. |
| include | console.h, vt.h, font.h, fb.h, driver.h, ipc.h | Core | Kernel console, primary framebuffer, device and IPC implementation interfaces. |
| include | fat.h, net.h, socket.h | Core | Storage and network data-path interfaces owned by the corresponding kernel subsystems. |
| include/drivers | armtimer.h, gicv2.h, pic_pit.h, pl011.h, ns16550_io.h, fdt.h, x86_platform.h, x86io.h | Core | Timer, interrupt, serial, boot discovery and privileged platform I/O. |
| include/drivers | ata_pio.h, virtio_mmio_blk.h, x86_nic.h | Core | Boot disk and NIC hardware interfaces used by the core data paths. |
| include/drivers | usb.h, hid_keyboard.h | Modular | Currently USB transport/class calls and bounded HID report parsing. Share only value-based messages across IPC. Keep kernel pointers and DMA metadata private. |
| include/uapi | chiffon_abi.h | Core | Kernel-owned versioned contract shared with all user clients. Keep existing values, structure sizes and semantics available. |
| userland | libchiffon.h | Modular | Unprivileged syscall wrappers and freestanding helpers used by clients and servers. |

## Integration outside the inventory roots

The Makefile compiles board code from device/ and user programs from env/<environment>/src. Demo's drvloader, fbserver and ipcdemo already run as processes. The kernel embeds the loader and framebuffer service executables when present. Demo also contains filesystem/network clients, memory tests and init. tools/release contains guest probes and the host smoke runner. These paths are part of the acceptance tests even though the five inventory roots do not contain them.

## Migration order and release gates

1. Establish service identity, cleanup, bounded IPC, user-buffer copying and default-deny resource grants. Test unauthorized operations and stale service handles in a guest on each architecture.
2. Move optional USB class processing and secondary input out of kernel context behind the device-owner contract. Bound the transfer sizes, validate endpoint identity, release pending calls on teardown and keep primary keyboard/console usable. Preserve storage mount/removal semantics. Raw bus-master access requires a separate enforceable confinement mechanism. [DMA trust boundary](../DMA.md)
3. Exercise file mutation, restart, replay and compaction; verify network requests through the existing syscall and NIC path. Check display service crash, error delivery and a new server registration. Confirm direct key press/release delivery on both primary keyboard transports.
4. Record the source revision, environment, emulator and command results for both architectures. Physical hardware remains a separate qualification target. [Release evidence](../RELEASE_EVIDENCE.md)
