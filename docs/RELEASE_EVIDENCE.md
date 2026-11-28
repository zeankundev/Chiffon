# Release evidence and recovery

Use this runbook with the [production readiness roadmap](../ROADMAP.md) and [threat model](THREAT_MODEL.md). The release smoke scope is the Demo environment with a trusted ext4 disk image and one vCPU. The two configurations are QEMU PC with ATA on x86_64 and QEMU virt with Cortex-A72, GICv2 and virtio-blk on AArch64. They use 256 MiB RAM and serial I/O. Production support requires the wider evidence listed in the roadmap.

## Generate and report evidence

### P5 release candidate verification

Tested source commit `5c734aaa46142a2b2dd6710ccd7089bc79898474` from a fresh Git archive in WSL. The working profiles were Demo, full, QEMU and ext4. Both targets used GCC 15.2, GNU binutils 2.46 and QEMU 10.2.1. The build promoted enabled kernel and user-program compiler warnings to errors. The build targets `help` and `new-env NAME=hybridcheck` also completed. Each board's own `make run` reached init, returned an architecture-matching `uname` and powered off.

| Repeatable check | x86_64 QEMU PC | AArch64 QEMU virt |
| --- | --- | --- |
| Clean build and versioned v1 ABI fixture | Pass | Pass |
| VM, clock, HID parser and unprivileged capability probes | Pass | Pass |
| Loader status, 32 IPC round trips, framebuffer start, ipcdemo console/display and three replies | Pass | Pass |
| Primary keyboard Ctrl-C press/release records and restoration to TTY mode | Pass, PS/2 | Pass, virtio input |
| HID server exit, stale handle and replacement | Pass, including xHCI keyboard/mouse and keyboard reattachment | Pass, service lifecycle |
| Gateway ping before and after framebuffer process failure and restart | Pass | Pass |
| File write, 3,000 rewrites, compaction, reboot, rename/unlink and hard stop following sync | Pass, whole disk and MBR | Pass, whole disk and MBR |

The host boot-memory fixture passed with AddressSanitizer and UndefinedBehaviorSanitizer. Guest reproduction uses the [P4 commands](hybrid/P4.md); MBR repeats use the [P2b procedure](hybrid/P2b.md). The [readiness workflow](../.github/workflows/readiness.yml) contains the exact target ABI and host sanitizer compiler commands. The [P5 review](hybrid/P5.md) records the scope and qualification limits.

| Artifact at test time | SHA-256 |
| --- | --- |
| x86_64 kernel | `e512632113b63dba7bb34183ec404528b39c49f6627190a8b3e0eec25945db18` |
| x86_64 pristine whole-disk ext4 image | `f723fdc495e50fa21dec30ecd109b8b28d43afaa109165c33772545acb86c029` |
| AArch64 kernel | `8e668fa27b28c48b7ca6e5eb87669d508f52d2dcc3121854fb02873ed5753921` |
| AArch64 pristine whole-disk ext4 image | `798ac95d14fe1a4fe317fc89ddd4c1cae742c5be51cd39fc45d0e43364bed5aa` |
| x86_64 BIOS initramfs ISO | `6276b399f048cb21d686493eb4b92a89bfe87ab0e820b51ee8b9ae7eb9ee6a48` |

### Optional x86_64 laptop image

The existing ISO target generated a BIOS El Torito image with GRUB Multiboot and a self-contained Demo initramfs. QEMU booted the actual ISO from its virtual CD-ROM, returned `Chiffon demo 0.6-salt x86_64` and powered off. Physical laptop boot, native UEFI boot and laptop device compatibility remain untested. The image's initramfs is volatile.

On an x86_64 Linux system with the Demo build dependencies, GRUB's `grub-mkrescue`, the i386-pc GRUB modules and xorriso, run these commands from a clean export of the tested source. At the guest prompt enter `uname`, then `poweroff`.

```sh
make -j8 ENV=demo ARCH=x86_64 DEVICE=qemu BUILD=build-iso CC='gcc -Werror' UCC='gcc -Werror' iso
image=build-iso/demo/x86_64-qemu/chiffon-x86_64.iso
qemu-system-x86_64 -m 256 -boot d -cdrom "$image" -display none -serial stdio -monitor none -no-reboot -nic none
```

For a physical test, use the ISO as bootable optical media on a machine that provides legacy BIOS or CSM boot. Select that media in the firmware's one-time boot menu and record the laptop model, firmware mode and guest behavior.

### Hybrid integration on Demo

The P4 run tested the kernel at `9104434` with the P4 guest probe and runners. Both clean Demo ext4 builds used GCC `-Werror` for the kernel and user programs. Both single-vCPU guests booted, ran the four base probes, exercised loader status, a 32-iteration IPC benchmark and ipcdemo, and shut down. Both passed the primary Ctrl-C test and TTY mode restoration, HID lifecycle, framebuffer failure and restart, gateway ICMP, and three-boot storage replay. The x86_64 guest also exercised USB keyboard/mouse delivery and keyboard unplug/replug. The ARM guest used virtio input. Reproduce with the [P4 commands](hybrid/P4.md).

The implemented boundary includes the v2 grant policy (`5e7adf0`), v3 USB HID transport (`91e2274`), bounded filesystem mounts (`8acb2b2`) and in-kernel network checks (`d0f104a`). The tested deployment uses trusted QEMU devices and backends. Physical laptop operation, DMA isolation, SMP stress and production deployment have no support claim from these runs.

### CI smoke procedure

1. A maintainer can manually dispatch **Release readiness smoke checks** for the candidate ref. The [workflow](../.github/workflows/readiness.yml) checks the v1 ABI fixture with both target compilers, runs the boot memory fixture on the Linux host, builds clean Demo images, and boots each image. It has no publishing permissions. The local contributor build remains the command in [.codex/AGENTS.md](../.codex/AGENTS.md).
2. Download the readiness artifact for each architecture, even on failure. A passing run has `PASS abi`, `PASS bootmem-compile`, `PASS bootmem`, and `PASS build` in `result.txt`, `PASS boot-smoke` in `boot/result.txt`, and `PASS vm-probe` in `boot/qemu.log`. Read `abi.log`, `bootmem-compile.log`, `bootmem.log`, `build.log`, `dependencies.log`, or `boot/qemu.log` for the first failing stage. A skipped step or missing result is incomplete evidence. ABI and build checks each have a five-minute deadline. The smoke check gives QEMU 60 seconds for the expected boot banner, init prompt, `uname`, reading `/etc/chiffon.ini`, running the VM probe, and power-off with a clean QEMU exit.
3. Record the candidate commit, release tag, ABI baseline, architecture, board, environment, QEMU version, full job URL, and result for each target. Preserve the artifact and record the image hashes from `boot/images.sha256`. Include the first failing stage and any recovery steps when filing a report. Mark runtime behavior verified only after examining both sets of results.

To repeat the smoke check with Demo assets that include `/bin/vm-probe`, a human operator can run the following on a Linux host with Python 3.11+ and the matching QEMU system emulators. Use a new evidence directory for each attempt.

```sh
python3 tools/release/boot_smoke.py x86_64 chiffon_x86_64.bin \
  chiffon-demo-x86_64.img build/evidence/amd64-repeat
python3 tools/release/boot_smoke.py aarch64 chiffon_aarch64.bin \
  chiffon-demo-aarch64.img build/evidence/arm64-repeat
```

The runner uses a temporary QEMU snapshot and writes the serial transcript and image SHA-256 digests to the evidence directory. If a timeout occurs, QEMU is stopped and the partial transcript is retained.

The boot memory fixture has 15 seconds to check range selection, clipping diagnostics, overlapping reservations, page alignment, and range capacity with AddressSanitizer and UndefinedBehaviorSanitizer enabled. It exercises the shared range code with synthetic maps. Firmware parsing and guest allocation require separate evidence. [Boot memory fixture](../tools/release/bootmem_test.c)

To run the VM probe directly, boot a newly built Demo or local milkos candidate in QEMU and enter `/bin/vm-probe` at the guest shell. Expect one `PASS vm-probe` line and a return to the prompt. A `FAIL vm-probe` line identifies the failing stage. The probe checks anonymous mapping, protection changes, write-and-execute denial from both syscalls, copying and modifying private data after fork, the child exit status, and unmapping. Its coverage is limited to data reads and writes and syscall results. Capture the serial output with the candidate commit and architecture.

## Human milkOS guest check, 2026-11-27

The maintainer confirmed that `make ENV=milkos BUSYBOX=$HOME/busybox/busybox` passed in their WSL instance using the working tree based on `2fadc20`, including its uncommitted integration changes. They booted the resulting x86_64 milkOS image with `run-gfx` and supplied a guest-console screenshot showing the following results with a 1024×768, 32-bit framebuffer.

| Guest action | Observed result |
| --- | --- |
| `vm-probe` | Printed `PASS vm-probe` and returned to the shell. |
| `drvloader status` | Loader ready, framebuffer on port 2, console on port 1. |
| `drvloader start fb` | Framebuffer server ready with PID 11 on port 34. |
| `drvloader test-crash` | The framebuffer replied before termination. The loader reported `-EPIPE`, the test printed its pass message, and the shell accepted another command. |
| `drvloader start fb` | Replacement server ready with PID 14 on port 66. |

The maintainer subsequently confirmed that the framebuffer client fix worked in the reported `fbview` and Doom use case. The change is recorded in `e0ba2aa`. This is application-level evidence for the milkOS graphics configuration. The report did not include separate AArch64 results.

## Human milkOS guest check, 2026-11-28

The maintainer reported a successful build of the clock and pathname changes. Their screenshot showed `PASS clock-probe`, `cat /etc/chiffon.ini` printing the configuration with `hostname = milkos`, and `cat /etc/chiffon.ini/` returning “Not a directory.” All three commands returned to the shell. The guest architecture and image hash were not supplied for this run.

## Removable USB keyboard check (pending)

The Demo serial smoke runner requires VM, clock, HID and capability probe markers plus the loader and IPC command checks before power-off. The x86_64 automated HID suite exercises an emulated keyboard and mouse. The manual steps below extend coverage to other devices and held-key removal.

Use the x86_64 QEMU board's graphical milkOS configuration. Its `xhci.0` bus starts empty. The built-in keyboard on the AArch64 QEMU board uses virtio-input. The xHCI driver currently binds devices directly connected to a root port and reads the first configuration, alternate setting zero. It supports HID boot keyboards and bounded keyboard/keypad report layouts including arrays, bitmaps, and report IDs. External hubs, vendor-defined key protocols, and arbitrary HID usages are outside this implementation.

1. In the guest, run `clock-probe` and `hid-probe`. Expect `PASS clock-probe` and `PASS hid-probe`. The latter exercises descriptor decoding in memory.
2. Switch from the QEMU guest display to the monitor (normally Ctrl+Alt+2). Enter `device_add usb-kbd,id=hotkbd,bus=xhci.0`, then `info usb`. Look for a USB keyboard. Return to the guest display (normally Ctrl+Alt+1) and look for the kernel's `keyboard interface` message in the console log.
3. Type `echo Usb123` in the guest window and press Enter. Expect exactly `Usb123`. Hold and release a letter at the shell prompt. It should repeat, then stop. Clear the line and run `cat /proc/keyboards`. A `usb-port-N` count should increase as you type. Counts are cumulative per root port for the boot.
4. In the monitor, enter `sendkey shift 30000`, then `device_del hotkbd` before the hold expires. Wait until `info usb` shows the keyboard has gone. Return to the guest and type `echo lower` using the board keyboard. Expect lowercase `lower` and a responsive shell.
5. Add the keyboard again with the same monitor command. Repeat step 3 and confirm the USB counter advances again. For a report-protocol keyboard or composite receiver, repeat the attach, typing and unplug sequence with that device connected directly to a supported xHCI root port. Record the device ID and which input checks passed.

## Version release inputs

- Treat [abi_v1.c](../tools/release/abi_v1.c) as the baseline for the checked syscall numbers, constants, structure sizes and offsets in the published [UAPI header](../include/uapi/chiffon_abi.h). Attach any deliberate incompatible change to a new baseline version and document the affected applications and migration. Keep older baselines available for review. The baseline checks declaration compatibility and the smoke test separately exercises a small part of the syscall dispatch.
- For every candidate, retain the source commit and tag, versioned environment configuration, compiler and binutils versions, emulator version, and tool package versions. Retain the exact BusyBox source revision, configuration and binary digest for any BusyBox-based asset. The smoke workflow records resolved packages. Reproducing official releases still requires pinning the release dependency versions, including BusyBox, and validating the resulting images.
- Prepare release notes with changes, known issues, tested configurations and supported limits. Verify downloaded images against the published `SHA256SUMS` obtained through a trusted channel. Retain a trusted previous release with its checksums. Release signing and rollback protection are open gates.

## Update and recover

1. Shut down the existing guest cleanly. Save its kernel, disk image and configuration together and record the checksums. Preserve the entire disk image because Chiffon stores the persistence log at its end.
2. Use a separate copy for the candidate. Rebuilding or replacing the image can discard the persistence log. Keep the old image and the candidate under distinct paths and boot the candidate with the same known-good board configuration. Run the smoke check on the matching Demo assets and independently verify any data migration on a copy.
3. If boot or data verification fails, stop the candidate, preserve its disk and serial log for diagnosis, and start the previous kernel with its matching previous disk and configuration. Check expected files and contents before using the restored system. Preserve the failed disk separately if any changes need manual recovery. Changes written only to the candidate are absent from the restored snapshot.

## Evidence still required

The workflow covers a versioned subset of ABI declarations and normal Demo boot behavior. Track kernel-wide warnings-as-errors, static analysis, fuzzing, in-kernel self-tests, hardware testing, SMP stress, storage durability, independent review, deployment diagnostics, authenticated upgrades and recovery in the [roadmap](../ROADMAP.md). The publishing workflow remains independent of this smoke workflow. The release decision requires review of candidate evidence before publication.
